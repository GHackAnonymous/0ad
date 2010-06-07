/* Copyright (C) 2010 Wildfire Games.
 * This file is part of 0 A.D.
 *
 * 0 A.D. is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * 0 A.D. is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with 0 A.D.  If not, see <http://www.gnu.org/licenses/>.
 */

// This module defines the table of all functions callable from JS.
// it's required by the interpreter; we make use of the opportunity to
// document them all in one spot. we thus obviate having to dig through
// all the other headers. most of the functions are implemented here;
// as for the rest, we only link to their docs (duplication is bad).

#include "precompiled.h"

#include "ScriptGlue.h"
#include "JSConversions.h"

#include "ScriptableComplex.inl"

#include "graphics/GameView.h"
#include "graphics/LightEnv.h"
#include "graphics/MapWriter.h"
#include "graphics/Unit.h"
#include "graphics/UnitManager.h"
#include "graphics/scripting/JSInterface_Camera.h"
#include "graphics/scripting/JSInterface_LightEnv.h"
#include "lib/timer.h"
#include "lib/svn_revision.h"
#include "lib/frequency_filter.h"
#include "maths/scripting/JSInterface_Vector3D.h"
#include "network/NetClient.h"
#include "network/NetServer.h"
#include "ps/CConsole.h"
#include "ps/CLogger.h"
#include "ps/CStr.h"
#include "ps/Game.h"
#include "ps/Globals.h"	// g_frequencyFilter
#include "ps/GameSetup/GameSetup.h"
#include "ps/Hotkey.h"
#include "ps/ProfileViewer.h"
#include "ps/World.h"
#include "ps/i18n.h"
#include "ps/scripting/JSCollection.h"
#include "ps/scripting/JSInterface_Console.h"
#include "ps/scripting/JSInterface_VFS.h"
#include "renderer/Renderer.h"
#include "renderer/SkyManager.h"
#include "scriptinterface/ScriptInterface.h"

#define LOG_CATEGORY L"script"
extern bool g_TerrainModified;


// rationale: the function table is now at the end of the source file to
// avoid the need for forward declarations for every function.

// all normal function wrappers have the following signature:
//   JSBool func(JSContext* cx, JSObject* globalObject, uintN argc, jsval* argv, jsval* rval);
// all property accessors have the following signature:
//   JSBool accessor(JSContext* cx, JSObject* globalObject, jsval id, jsval* vp);


//-----------------------------------------------------------------------------
// Output
//-----------------------------------------------------------------------------

// Write values to the log file.
// params: any number of any type.
// returns:
// notes:
// - Each argument is converted to a string and then written to the log.
// - Output is in NORMAL style (see LOG).
JSBool WriteLog(JSContext* cx, JSObject*, uintN argc, jsval* argv, jsval* rval)
{
	JSU_REQUIRE_PARAMS(1);

	CStrW logMessage;

	for (int i = 0; i < (int)argc; i++)
	{
		try
		{
			CStrW arg = g_ScriptingHost.ValueToUCString( argv[i] );
			logMessage += arg;
		}
		catch( PSERROR_Scripting_ConversionFailed )
		{
			// Do nothing.
		}
	}

	LOG(CLogger::Normal, LOG_CATEGORY, L"%ls", logMessage.c_str());

	*rval = JSVAL_TRUE;
	return JS_TRUE;
}


//-----------------------------------------------------------------------------
// Timer
//-----------------------------------------------------------------------------

// Set the simulation rate scalar-time becomes time * SimRate.
// Params: rate [float] : sets SimRate
JSBool SetSimRate(JSContext* cx, JSObject*, uintN argc, jsval* argv, jsval* rval)
{
	JSU_REQUIRE_PARAMS(1);

	g_Game->SetSimRate( ToPrimitive<float>(argv[0]) );
	return JS_TRUE;
}

// Script profiling functions: Begin timing a piece of code with StartJsTimer(num)
// and stop timing with StopJsTimer(num). The results will be printed to stdout
// when the game exits.

static const size_t MAX_JS_TIMERS = 20;
static TimerUnit js_start_times[MAX_JS_TIMERS];
static TimerUnit js_timer_overhead;
static TimerClient js_timer_clients[MAX_JS_TIMERS];
static wchar_t js_timer_descriptions_buf[MAX_JS_TIMERS * 12];	// depends on MAX_JS_TIMERS and format string below

static void InitJsTimers()
{
	wchar_t* pos = js_timer_descriptions_buf;
	for(size_t i = 0; i < MAX_JS_TIMERS; i++)
	{
		const wchar_t* description = pos;
		pos += swprintf_s(pos, 12, L"js_timer %d", (int)i)+1;
		timer_AddClient(&js_timer_clients[i], description);
	}

	// call several times to get a good approximation of 'hot' performance.
	// note: don't use a separate timer slot to warm up and then judge
	// overhead from another: that causes worse results (probably some
	// caching effects inside JS, but I don't entirely understand why).
	static const char* calibration_script =
		"startXTimer(0);\n"
		"stopXTimer (0);\n"
		"\n";
	g_ScriptingHost.RunMemScript(calibration_script, strlen(calibration_script));
	// slight hack: call RunMemScript twice because we can't average several
	// TimerUnit values because there's no operator/. this way is better anyway
	// because it hopefully avoids the one-time JS init overhead.
	g_ScriptingHost.RunMemScript(calibration_script, strlen(calibration_script));
	js_timer_overhead = js_timer_clients[0].sum;
	js_timer_clients[0].sum.SetToZero();
}

JSBool StartJsTimer(JSContext* cx, JSObject*, uintN argc, jsval* argv, jsval* rval)
{
	ONCE(InitJsTimers());

	JSU_REQUIRE_PARAMS(1);
	size_t slot = ToPrimitive<size_t>(argv[0]);
	if (slot >= MAX_JS_TIMERS)
		return JS_FALSE;

	js_start_times[slot].SetFromTimer();
	return JS_TRUE;
}


JSBool StopJsTimer(JSContext* cx, JSObject*, uintN argc, jsval* argv, jsval* rval)
{
	JSU_REQUIRE_PARAMS(1);
	size_t slot = ToPrimitive<size_t>(argv[0]);
	if (slot >= MAX_JS_TIMERS)
		return JS_FALSE;

	TimerUnit now;
	now.SetFromTimer();
	now.Subtract(js_timer_overhead);
	timer_BillClient(&js_timer_clients[slot], js_start_times[slot], now);
	js_start_times[slot].SetToZero();
	return JS_TRUE;
}


//-----------------------------------------------------------------------------
// Game Setup
//-----------------------------------------------------------------------------

// Create a new network server object.
// params:
// returns: net server object
JSBool CreateServer(JSContext* cx, JSObject*, uintN argc, jsval* argv, jsval* rval)
{
	JSU_REQUIRE_NO_PARAMS();

	if( !g_Game )
		g_Game = new CGame();
	if( !g_NetServer )
		g_NetServer = new CNetServer(g_Game, &g_GameAttributes);

	*rval = OBJECT_TO_JSVAL(g_NetServer->GetScript());
	return( JS_TRUE );
}


// Create a new network client object.
// params:
// returns: net client object
JSBool CreateClient(JSContext* cx, JSObject*, uintN argc, jsval* argv, jsval* rval)
{
	JSU_REQUIRE_NO_PARAMS();

	if( !g_Game )
		g_Game = new CGame();
	if( !g_NetClient )
		g_NetClient = new CNetClient(g_Game, &g_GameAttributes);

	*rval = OBJECT_TO_JSVAL(g_NetClient->GetScript());
	return( JS_TRUE );
}


// Begin the process of starting a game.
// params:
// returns: success [bool]
// notes:
// - Performs necessary initialization while calling back into the
//   main loop, so the game remains responsive to display+user input.
// - When complete, the engine calls the reallyStartGame JS function.
// TODO: Replace StartGame with Create(Game|Server|Client)/game.start() -
//   after merging CGame and CGameAttributes
JSBool StartGame(JSContext* cx, JSObject*, uintN argc, jsval* argv, jsval* rval)
{
	JSU_REQUIRE_NO_PARAMS();

	*rval = BOOLEAN_TO_JSVAL(JS_TRUE);

	// Hosted MP Game
	if (g_NetServer) 
	{
		*rval = BOOLEAN_TO_JSVAL(g_NetServer->StartGame() == 0);
	}
	// Joined MP Game
	else if (g_NetClient)
	{
		*rval = BOOLEAN_TO_JSVAL(g_NetClient->StartGame() == 0);
	}
	// Start an SP Game Session
	else if (!g_Game)
	{
		g_Game = new CGame();
		PSRETURN ret = g_Game->StartGame(&g_GameAttributes);
		if (ret != PSRETURN_OK)
		{
			// Failed to start the game - destroy it, and return false

			delete g_Game;
			g_Game = NULL;

			*rval = BOOLEAN_TO_JSVAL(JS_FALSE);
			return( JS_TRUE );
		}
	}
	else
	{
		*rval = BOOLEAN_TO_JSVAL(JS_FALSE);
	}

	return( JS_TRUE );
}


// Immediately ends the current game (if any).
// params:
// returns:
JSBool EndGame(JSContext* cx, JSObject*, uintN argc, jsval* argv, jsval* rval)
{
	JSU_REQUIRE_NO_PARAMS();

	EndGame();
	return JS_TRUE;
}

JSBool GetGameMode(JSContext* cx, JSObject*, uintN argc, jsval* argv, jsval* rval)
{
	JSU_REQUIRE_NO_PARAMS();

	*rval = ToJSVal( g_GameAttributes.GetGameMode() );
	return JS_TRUE;
}

//-----------------------------------------------------------------------------
// Internationalization
//-----------------------------------------------------------------------------

// these remain here instead of in the i18n tree because they are
// really related to the engine's use of them, as opposed to i18n itself.
// contrariwise, translate() cannot be moved here because that would
// make i18n dependent on this code and therefore harder to reuse.

// Replaces the current language (locale) with a new one.
// params: language id [string] as in I18n::LoadLanguage
// returns:
JSBool LoadLanguage(JSContext* cx, JSObject*, uintN argc, jsval* argv, jsval* rval)
{
	JSU_REQUIRE_PARAMS(1);

	CStr lang = g_ScriptingHost.ValueToString(argv[0]);
	I18n::LoadLanguage(lang);

	return JS_TRUE;
}


// Return identifier of the current language (locale) in use.
// params:
// returns: language id [string] as in I18n::LoadLanguage
JSBool GetLanguageID(JSContext* cx, JSObject*, uintN argc, jsval* argv, jsval* rval)
{
	JSU_REQUIRE_NO_PARAMS();
	*rval = JSVAL_NULL;

	JSString* s = JS_NewStringCopyZ(cx, I18n::CurrentLanguageName());
	if (!s)
	{
		JS_ReportError(cx, "Error creating string");
		return JS_FALSE;
	}
	*rval = STRING_TO_JSVAL(s);
	return JS_TRUE;
}


//-----------------------------------------------------------------------------
// Debug
//-----------------------------------------------------------------------------


// Deliberately cause the game to crash.
// params:
// returns:
// notes:
// - currently implemented via access violation (read of address 0)
// - useful for testing the crashlog/stack trace code.
JSBool ProvokeCrash(JSContext* cx, JSObject*, uintN argc, jsval* argv, jsval* rval)
{
	JSU_REQUIRE_NO_PARAMS();

	MICROLOG(L"Crashing at user's request.");
	return *(JSBool*)0;
}


// Force a JS garbage collection cycle to take place immediately.
// params:
// returns: true [bool]
// notes:
// - writes an indication of how long this took to the console.
JSBool ForceGarbageCollection(JSContext* cx, JSObject* UNUSED(obj), uintN argc, jsval* argv, jsval* rval)
{
	JSU_REQUIRE_NO_PARAMS();

	double time = timer_Time();
	JS_GC(cx);
	time = timer_Time() - time;
	g_Console->InsertMessage(L"Garbage collection completed in: %f", time);
	*rval = JSVAL_TRUE;
	return JS_TRUE ;
}



//-----------------------------------------------------------------------------
// Misc. Engine Interface
//-----------------------------------------------------------------------------

// Return the global frames-per-second value.
// params:
// returns: FPS [int]
// notes:
// - This value is recalculated once a frame. We take special care to
//   filter it, so it is both accurate and free of jitter.
JSBool GetFps( JSContext* cx, JSObject*, uintN argc, jsval* argv, jsval* rval )
{
	JSU_REQUIRE_NO_PARAMS();
	*rval = INT_TO_JSVAL(g_frequencyFilter->StableFrequency());
	return JS_TRUE;
}


// Cause the game to exit gracefully.
// params:
// returns:
// notes:
// - Exit happens after the current main loop iteration ends
//   (since this only sets a flag telling it to end)
JSBool ExitProgram( JSContext* cx, JSObject*, uintN argc, jsval* argv, jsval* rval )
{
	JSU_REQUIRE_NO_PARAMS();

	kill_mainloop();
	return JS_TRUE;
}


// Write an indication of total video RAM to console.
// params:
// returns:
// notes:
// - May not be supported on all platforms.
// - Only a rough approximation; do not base low-level decisions
//   ("should I allocate one more texture?") on this.
JSBool WriteVideoMemToConsole( JSContext* cx, JSObject*, uintN argc, jsval* argv, jsval* rval )
{
	JSU_REQUIRE_NO_PARAMS();

	const SDL_VideoInfo* videoInfo = SDL_GetVideoInfo();
	g_Console->InsertMessage(L"VRAM: total %d", videoInfo->video_mem);
	return JS_TRUE;
}


// Change the mouse cursor.
// params: cursor name [string] (i.e. basename of definition file and texture)
// returns:
// notes:
// - Cursors are stored in "art\textures\cursors"
JSBool SetCursor( JSContext* cx, JSObject*, uintN argc, jsval* argv, jsval* rval )
{
	JSU_REQUIRE_PARAMS(1);
	g_CursorName = g_ScriptingHost.ValueToUCString(argv[0]);
	return JS_TRUE;
}

JSBool GetCursorName( JSContext* UNUSED(cx), JSObject*, uintN UNUSED(argc), jsval* UNUSED(argv), jsval* rval )
{
	*rval = ToJSVal(g_CursorName);
	return JS_TRUE;
}

// Trigger a rewrite of all maps.
// params:
// returns:
// notes:
// - Usefulness is unclear. If you need it, consider renaming this and updating the docs.
JSBool _RewriteMaps( JSContext* cx, JSObject*, uintN argc, jsval* argv, jsval* rval )
{
	JSU_REQUIRE_NO_PARAMS();

	g_Game->GetWorld()->RewriteMap();
	return JS_TRUE;
}


// Change the LOD bias.
// params: LOD bias [float]
// returns:
// notes:
// - value is as required by GL_TEXTURE_LOD_BIAS.
// - useful for adjusting image "sharpness" (since it affects which mipmap level is chosen)
JSBool _LodBias( JSContext* cx, JSObject*, uintN argc, jsval* argv, jsval* rval )
{
	JSU_REQUIRE_PARAMS(1);

	g_Renderer.SetOptionFloat(CRenderer::OPT_LODBIAS, ToPrimitive<float>(argv[0]));
	return JS_TRUE;
}


// Focus the game camera on a given position.
// params: target position vector [CVector3D]
// returns: success [bool]
JSBool SetCameraTarget( JSContext* cx, JSObject* UNUSED(obj), uintN argc, jsval* argv, jsval* rval )
{
	JSU_REQUIRE_PARAMS(1);
	*rval = JSVAL_NULL;

	CVector3D* target = ToNative<CVector3D>( argv[0] );
	if(!target)
	{
		JS_ReportError( cx, "Invalid camera target" );
		return( JS_TRUE );
	}
	g_Game->GetView()->SetCameraTarget( *target );

	*rval = JSVAL_TRUE;
	return( JS_TRUE );
}


//-----------------------------------------------------------------------------
// Miscellany
//-----------------------------------------------------------------------------

// Return the date/time at which the current executable was compiled.
// params: none (-> "date time (svn revision)") OR an integer specifying
//   what to display: 0 for date, 1 for time, 2 for svn revision
// returns: string with the requested timestamp info
// notes:
// - Displayed on main menu screen; tells non-programmers which auto-build
//   they are running. Could also be determined via .EXE file properties,
//   but that's a bit more trouble.
// - To be exact, the date/time returned is when scriptglue.cpp was
//   last compiled, but the auto-build does full rebuilds.
// - svn revision is generated by calling svnversion and cached in
//   lib/svn_revision.cpp. it is useful to know when attempting to
//   reproduce bugs (the main EXE and PDB should be temporarily reverted to
//   that revision so that they match user-submitted crashdumps).
JSBool GetBuildTimestamp( JSContext* cx, JSObject*, uintN argc, jsval* argv, jsval* rval )
{
	JSU_REQUIRE_MAX_PARAMS(1);

	char buf[200];

	// see function documentation
	const int mode = argc? JSVAL_TO_INT(argv[0]) : -1;
	switch(mode)
	{
	case -1:
		sprintf_s(buf, ARRAY_SIZE(buf), "%s %s (%ls)", __DATE__, __TIME__, svn_revision);
		break;
	case 0:
		sprintf_s(buf, ARRAY_SIZE(buf), "%s", __DATE__);
		break;
	case 1:
		sprintf_s(buf, ARRAY_SIZE(buf), "%s", __TIME__);
		break;
	case 2:
		sprintf_s(buf, ARRAY_SIZE(buf), "%ls", svn_revision);
		break;
	}

	*rval = STRING_TO_JSVAL(JS_NewStringCopyZ(cx, buf));
	return JS_TRUE;
}


// Return distance between 2 points.
// params: 2 position vectors [CVector3D]
// returns: Euclidean distance [float]
JSBool ComputeDistanceBetweenTwoPoints( JSContext* cx, JSObject* UNUSED(obj), uintN argc, jsval* argv, jsval* rval )
{
	JSU_REQUIRE_PARAMS(2);

	CVector3D* a = ToNative<CVector3D>( argv[0] );
	CVector3D* b = ToNative<CVector3D>( argv[1] );
	float dist = ( *a - *b ).Length();
	*rval = ToJSVal( dist );
	return( JS_TRUE );
}


// Returns the global object.
// params:
// returns: global object
// notes:
// - Useful for accessing an object from another scope.
JSBool GetGlobal( JSContext* cx, JSObject* globalObject, uintN argc, jsval* argv, jsval* rval )
{
	JSU_REQUIRE_NO_PARAMS();

	*rval = OBJECT_TO_JSVAL( globalObject );
	return( JS_TRUE );
}

// Saves the current profiling data to the logs/profile.txt file
JSBool SaveProfileData( JSContext* cx, JSObject* UNUSED(globalObject), uintN argc, jsval* argv, jsval* rval )
{
	JSU_REQUIRE_NO_PARAMS();
	g_ProfileViewer.SaveToFile();
	return( JS_TRUE );
}

// Toggles drawing the sky
JSBool ToggleSky( JSContext* cx, JSObject* UNUSED(globalObject), uintN argc, jsval* argv, jsval* rval )
{
	JSU_REQUIRE_NO_PARAMS();
	g_Renderer.GetSkyManager()->m_RenderSky = !g_Renderer.GetSkyManager()->m_RenderSky;
	*rval = JSVAL_VOID;
	return( JS_TRUE );
}

//-----------------------------------------------------------------------------

// Is the game paused?
JSBool IsPaused( JSContext* cx, JSObject* UNUSED(globalObject), uintN argc, jsval* argv, jsval* rval )
{
	JSU_REQUIRE_NO_PARAMS();

	if( !g_Game )
	{
		JS_ReportError( cx, "Game is not started" );
		return JS_FALSE;
	}

	*rval = g_Game->m_Paused ? JSVAL_TRUE : JSVAL_FALSE;
	return JS_TRUE ;
}

// Pause/unpause the game
JSBool SetPaused( JSContext* cx, JSObject* UNUSED(globalObject), uintN argc, jsval* argv, jsval* rval )
{
	JSU_REQUIRE_PARAMS( 1 );

	if( !g_Game )
	{
		JS_ReportError( cx, "Game is not started" );
		return JS_FALSE;
	}

	try
	{
		g_Game->m_Paused = ToPrimitive<bool>( argv[0] );
	}
	catch( PSERROR_Scripting_ConversionFailed )
	{
		JS_ReportError( cx, "Invalid parameter to SetPaused" );
	}

	return  JS_TRUE;
}

// Reveal map
JSBool RevealMap( JSContext* cx, JSObject* UNUSED(globalObject), uintN argc, jsval* argv, jsval* rval )
{
	JSU_REQUIRE_MAX_PARAMS(1);

	int newValue;
	if(argc == 0)
		newValue = LOS_SETTING_ALL_VISIBLE;
	else if(!ToPrimitive( g_ScriptingHost.GetContext(), argv[0], newValue ) || newValue > 2)
	{
		JS_ReportError( cx, "Invalid argument (should be 0, 1 or 2)" );
		*rval = JSVAL_VOID;
		return( JS_FALSE );
	}

	g_Game->GetWorld()->GetLOSManager()->m_LOSSetting = (ELOSSetting)newValue;
	*rval = JSVAL_VOID;
	return( JS_TRUE );
}

/**
 * isGameRunning
 * @return bool
 */
JSBool isGameRunning( JSContext* cx, JSObject* UNUSED(globalObject), uintN argc, jsval* argv, jsval* rval )
{
	JSU_REQUIRE_NO_PARAMS();
	
	if (g_Game && g_Game->IsGameStarted())
	{
		*rval = JSVAL_TRUE;
	}
	else
	{
		*rval = JSVAL_FALSE;
	}
	
	return JS_TRUE;
}



//-----------------------------------------------------------------------------
// function table
//-----------------------------------------------------------------------------

// the JS interpreter expects the table to contain 5-tuples as follows:
// - name the function will be called as from script;
// - function which will be called;
// - number of arguments this function expects
// - Flags (deprecated, always zero)
// - Extra (reserved for future use, always zero)
//
// we simplify this a bit with a macro:
#define JS_FUNC(script_name, cpp_function, min_params) { script_name, cpp_function, min_params, 0, 0 },

JSFunctionSpec ScriptFunctionTable[] =
{
	// Console
	JS_FUNC("writeConsole", JSI_Console::writeConsole, 1)	// external

	// Camera
	JS_FUNC("setCameraTarget", SetCameraTarget, 1)

	// Sky
	JS_FUNC("toggleSky", ToggleSky, 0)

	// Timer
	JS_FUNC("setSimRate", SetSimRate, 1)

	// Profiling
	JS_FUNC("startXTimer", StartJsTimer, 1)
	JS_FUNC("stopXTimer", StopJsTimer, 1)

	// Game Setup
	JS_FUNC("startGame", StartGame, 0)
	JS_FUNC("endGame", EndGame, 0)
	JS_FUNC("getGameMode", GetGameMode, 0)
	JS_FUNC("createClient", CreateClient, 0)
	JS_FUNC("createServer", CreateServer, 0)

	// VFS (external)
	JS_FUNC("buildDirEntList", JSI_VFS::BuildDirEntList, 1)
	JS_FUNC("getFileMTime", JSI_VFS::GetFileMTime, 1)
	JS_FUNC("getFileSize", JSI_VFS::GetFileSize, 1)
	JS_FUNC("readFile", JSI_VFS::ReadFile, 1)
	JS_FUNC("readFileLines", JSI_VFS::ReadFileLines, 1)
	JS_FUNC("archiveBuilderCancel", JSI_VFS::ArchiveBuilderCancel, 1)

	// Internationalization
	JS_FUNC("loadLanguage", LoadLanguage, 1)
	JS_FUNC("getLanguageID", GetLanguageID, 0)
	// note: i18n/ScriptInterface.cpp registers translate() itself.
	// rationale: see implementation section above.

	// Debug
	JS_FUNC("crash", ProvokeCrash, 0)
	JS_FUNC("forceGC", ForceGarbageCollection, 0)
	JS_FUNC("revealMap", RevealMap, 1)

	// Misc. Engine Interface
	JS_FUNC("writeLog", WriteLog, 1)
	JS_FUNC("exit", ExitProgram, 0)
	JS_FUNC("isPaused", IsPaused, 0)
	JS_FUNC("setPaused", SetPaused, 1)
	JS_FUNC("vmem", WriteVideoMemToConsole, 0)
	JS_FUNC("_rewriteMaps", _RewriteMaps, 0)
	JS_FUNC("_lodBias", _LodBias, 0)
	JS_FUNC("setCursor", SetCursor, 1)
	JS_FUNC("getCursorName", GetCursorName, 0)
	JS_FUNC("getFPS", GetFps, 0)
	JS_FUNC("isGameRunning", isGameRunning, 0)

	// Miscellany
	JS_FUNC("v3dist", ComputeDistanceBetweenTwoPoints, 2)
	JS_FUNC("buildTime", GetBuildTimestamp, 0)
	JS_FUNC("getGlobal", GetGlobal, 0)
	JS_FUNC("saveProfileData", SaveProfileData, 0)

	// end of table marker
	{0, 0, 0, 0, 0}
};
#undef JS_FUNC


//-----------------------------------------------------------------------------
// property accessors
//-----------------------------------------------------------------------------

JSBool GetPlayerSet( JSContext* UNUSED(cx), JSObject* UNUSED(obj), jsval UNUSED(id), jsval* vp )
{
	std::vector<CPlayer*>* players = g_Game->GetPlayers();

	*vp = OBJECT_TO_JSVAL( PlayerCollection::Create( *players ) );

	return( JS_TRUE );
}


JSBool GetLocalPlayer( JSContext* UNUSED(cx), JSObject* UNUSED(obj), jsval UNUSED(id), jsval* vp )
{
	*vp = OBJECT_TO_JSVAL( g_Game->GetLocalPlayer()->GetScript() );
	return( JS_TRUE );
}


JSBool GetGaiaPlayer( JSContext* UNUSED(cx), JSObject* UNUSED(obj), jsval UNUSED(id), jsval* vp )
{
	*vp = OBJECT_TO_JSVAL( g_Game->GetPlayer( 0 )->GetScript() );
	return( JS_TRUE );
}


JSBool SetLocalPlayer( JSContext* cx, JSObject* UNUSED(obj), jsval UNUSED(id), jsval* vp )
{
	CPlayer* newLocalPlayer = ToNative<CPlayer>( *vp );

	if( !newLocalPlayer )
	{
		JS_ReportError( cx, "Not a valid Player." );
		return( JS_TRUE );
	}

	g_Game->SetLocalPlayer( newLocalPlayer );
	return( JS_TRUE );
}


JSBool GetGameView( JSContext* UNUSED(cx), JSObject* UNUSED(obj), jsval UNUSED(id), jsval* vp )
{
	if (g_Game)
		*vp = OBJECT_TO_JSVAL( g_Game->GetView()->GetScript() );
	else
		*vp = JSVAL_NULL;
	return( JS_TRUE );
}


JSBool GetRenderer( JSContext* UNUSED(cx), JSObject* UNUSED(obj), jsval UNUSED(id), jsval* vp )
{
	if (CRenderer::IsInitialised())
		*vp = OBJECT_TO_JSVAL( g_Renderer.GetScript() );
	else
		*vp = JSVAL_NULL;
	return( JS_TRUE );
}


enum ScriptGlobalTinyIDs
{
	GLOBAL_SELECTION,
	GLOBAL_GROUPSARRAY,
	GLOBAL_CAMERA,
	GLOBAL_CONSOLE,
	GLOBAL_LIGHTENV
};

JSPropertySpec ScriptGlobalTable[] =
{
	{ "camera"     , GLOBAL_CAMERA,      JSPROP_PERMANENT, JSI_Camera::getCamera, JSI_Camera::setCamera },
	{ "console"    , GLOBAL_CONSOLE,     JSPROP_PERMANENT|JSPROP_READONLY, JSI_Console::getConsole, 0 },
	{ "lightenv"   , GLOBAL_LIGHTENV,    JSPROP_PERMANENT, JSI_LightEnv::getLightEnv, JSI_LightEnv::setLightEnv },
	{ "players"    , 0,                  JSPROP_PERMANENT|JSPROP_READONLY, GetPlayerSet, 0 },
	{ "localPlayer", 0,                  JSPROP_PERMANENT, GetLocalPlayer, SetLocalPlayer },
	{ "gaiaPlayer" , 0,                  JSPROP_PERMANENT|JSPROP_READONLY, GetGaiaPlayer, 0 },
	{ "gameView"   , 0,                  JSPROP_PERMANENT|JSPROP_READONLY, GetGameView, 0 },
	{ "renderer"   , 0,                  JSPROP_PERMANENT|JSPROP_READONLY, GetRenderer, 0 },

	// end of table marker
	{ 0, 0, 0, 0, 0 },
};
