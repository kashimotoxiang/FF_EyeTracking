
/*******************************************************************************

INTEL CORPORATION PROPRIETARY INFORMATION
This software is supplied under the terms of a license agreement or nondisclosure
agreement with Intel Corporation and may not be copied or disclosed except in
accordance with the terms of that agreement
Copyright(c) 2012-2013 Intel Corporation. All Rights Reserved.

*******************************************************************************/
#include <Windows.h>
#include <WindowsX.h>
#include <commctrl.h>
#include <io.h>
#include <map>
#include "math.h"
#include "resource.h"
#include "pxcfacemodule.h"
#include "pxcfacedata.h"
#include "pxcvideomodule.h"
#include "pxcfaceconfiguration.h"
#include "pxcmetadata.h"
#include "service/pxcsessionservice.h"
#include "FaceTrackingFrameRateCalculator.h"
#include "FaceTrackingRendererManager.h"
#include "FaceTrackingRenderer2D.h"
#include "FaceTrackingRenderer3D.h"
#include "FaceTrackingUtilities.h"
#include "FaceTrackingProcessor.h"
#include "Strsafe.h"
#include <string.h>

WCHAR user_name[40];
pxcCHAR calibFileName[1024] = { 0 };
pxcCHAR rssdkFileName[1024] = { 0 };
PXCSession* session = NULL;
FaceTrackingRendererManager* renderer = NULL;
FaceTrackingProcessor* processor = NULL;

HANDLE ghMutex = NULL;
HWND ghWnd = NULL;
HWND ghWndEyeBack = NULL;
HWND ghWndEyePoint = NULL;
HINSTANCE ghInstance = NULL;

volatile bool isRunning = false;
volatile bool isStopped = false;
volatile bool isPaused = false;
volatile bool isActiveApp = true;
volatile bool isLoadCalibFile = false;

static int controls[] = { IDC_SCALE, IDC_LOCATION, IDC_LANDMARK, IDC_POSE, ID_START, ID_STOP, ID_LOAD_CALIB, ID_NEW_CALIB, IDC_STATIC2, IDC_LIST1 };
static RECT layout[3 + sizeof(controls) / sizeof(controls[0])];

volatile int eye_point_x = 2000;
volatile int eye_point_y = 2000;

const int max_path_length = 1024;

int cursor_pos_x = 0;
int cursor_pos_y = 0;
CalibMode modeWork = mode_calib;

extern short calibBuffersize;
extern unsigned char* calibBuffer;
extern int calib_status;
extern int dominant_eye;

typedef DWORD (WINAPI *make_layered)(HWND, DWORD, BYTE, DWORD);
static make_layered set_layered_window = NULL;
static BOOL dll_initialized = FALSE;

bool make_transparent(HWND hWnd) {

	if (!dll_initialized) {

		HMODULE hLib = LoadLibraryA ("user32");
		set_layered_window = (make_layered) GetProcAddress(hLib, "SetLayeredWindowAttributes");
		dll_initialized = TRUE;

	}

	if (set_layered_window == NULL) return FALSE;
	SetLastError(0);

	SetWindowLong(hWnd, GWL_EXSTYLE , GetWindowLong(hWnd, GWL_EXSTYLE) | WS_EX_LAYERED);
	if (GetLastError()) return FALSE;

	return set_layered_window(hWnd, RGB(0,0,0), 127, LWA_COLORKEY|LWA_ALPHA) != NULL;

}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {

	switch (message)
	{

	case WM_KEYDOWN: // exit calibration

		switch (wParam) {

		case VK_ESCAPE:
			PostMessage(ghWnd, WM_COMMAND, ID_STOP, 0);
			CloseWindow(hWnd);
			break;

		}

	}

	// default message handling

	if (message) return DefWindowProc(hWnd, message, wParam, lParam);
	else return 0;

}

ATOM MyRegisterClass(HINSTANCE hInstance, DWORD color, WCHAR* name) {

	WNDCLASSEX wcex;

	wcex.cbSize = sizeof(WNDCLASSEX);

	wcex.style			= CS_HREDRAW | CS_VREDRAW;
	wcex.lpfnWndProc	= WndProc;
	wcex.cbClsExtra		= 0;
	wcex.cbWndExtra		= 0;
	wcex.hInstance		= hInstance;
	wcex.hIcon			= NULL;
	wcex.hCursor		= LoadCursor(NULL, IDC_ARROW);
	wcex.hbrBackground	= (HBRUSH)CreateSolidBrush(color);
	wcex.lpszMenuName	= NULL;
	wcex.lpszClassName	= name;
	wcex.hIconSm		= NULL;

	return RegisterClassEx(&wcex);

}

bool InitSimpleWindow(HWND* hwnd, int size, DWORD color, WCHAR* name) {

	if (hwnd == NULL) return false;

	// create transparent eye point window

	if (*hwnd == NULL) {

		MyRegisterClass(ghInstance, color, name);

		*hwnd = CreateWindow(name, name, WS_POPUP,
			-500, -500, size, size, ghWndEyeBack, NULL, ghInstance, NULL);

		if (!hwnd) return false;

	}

	ShowWindow(*hwnd, SW_SHOW);
	UpdateWindow(*hwnd);

	return true;

}

bool InitTransWindow(HWND* hwnd, int size, DWORD color, WCHAR* name) {

	if (hwnd == NULL) return false;

	// create transparent eye point window

	if (*hwnd == NULL) {

		MyRegisterClass(ghInstance, color, name);

		*hwnd = CreateWindow(name, name, WS_POPUP,
			-500, -500, size, size, ghWndEyeBack, NULL, ghInstance, NULL);

		if (!hwnd) return false;

	}

	make_transparent(*hwnd);
	ShowWindow(*hwnd, SW_SHOW);
	UpdateWindow(*hwnd);

	return true;

}

bool InitBackWindow(HWND* hwnd, DWORD color, WCHAR* name) {

	if (hwnd == NULL) return false;

	// create transparent eye point window

	if (*hwnd == NULL) {

		MyRegisterClass(ghInstance, color, name);

		RECT rc;
		const HWND hDesktop = GetDesktopWindow();
		GetWindowRect(hDesktop, &rc);

		*hwnd = CreateWindow(name, name, WS_POPUP,
			0, 0, rc.right, rc.bottom, ghWnd, NULL, ghInstance, NULL);

		if (!hwnd) return false;

	}

	// hide back window at first enable when API loaded

	ShowWindow(*hwnd, SW_HIDE);
	UpdateWindow(*hwnd);

	return true;

}

void CloseTransWindow(HWND* hwnd) {

	// close EyePoint window

	if (*hwnd) {

		DestroyWindow(*hwnd);
		*hwnd = NULL;

	}

}

void CloseCalibWindows() {

	// close calibration windows

	CloseTransWindow(&ghWndEyeBack);
	CloseTransWindow(&ghWndEyePoint);

}

void EnableBackWindow() {

	// show message box as latest point

	MessageBoxA(ghWnd, "Calibration is required, please keep your head fixed and look at the red square that appears on the screen",
		"Calibration Required", MB_OK | MB_ICONINFORMATION);

	// enable back window
	
	ShowWindow(ghWndEyeBack, SW_SHOW);
	UpdateWindow(ghWndEyeBack);

}

void InitCalibWindows(CalibMode mode) {

	// close the old windows

	CloseCalibWindows();

	// init calibration windows

	modeWork = mode; 
	switch (mode) {

	case mode_calib:
		InitBackWindow(&ghWndEyeBack, RGB(240, 240, 240), L"Background");
		InitSimpleWindow(&ghWndEyePoint, 35, RGB(255, 0, 0), L"EyePoint1");  // 35-50
		break;

	case mode_live:
		InitTransWindow(&ghWndEyePoint, 100, RGB(255, 255, 0), L"EyePoint2");
		break;

	case mode_playback:
		InitTransWindow(&ghWndEyePoint, 100, RGB(0, 255, 0), L"EyePoint3");
		break;

	case mode_record:
		InitSimpleWindow(&ghWndEyePoint, 35, RGB(255, 0, 0), L"EyePoint4");
		break;

	}

	// focus on main window
	
	SetFocus(ghWnd);

}

void UpdateTracking() {

	// set position of gaze point

	if (ghWndEyePoint) {

		RECT rc;
		GetWindowRect(ghWndEyePoint, &rc);

		int width = rc.right - rc.left;
		int height = rc.bottom - rc.top;

		if (modeWork == mode_record) {

			POINT cursorPos;
			GetCursorPos(&cursorPos);
			SetWindowPos(ghWndEyePoint, NULL, cursorPos.x - width/2, cursorPos.y - height/2, width, height, NULL);

		} else {

			SetWindowPos(ghWndEyePoint, NULL, eye_point_x - width/2, eye_point_y - height/2, width, height, NULL);

		}

	}

}

std::map<int, PXCFaceConfiguration::TrackingModeType> CreateProfileMap() {

	std::map<int, PXCFaceConfiguration::TrackingModeType> map;
	map[0] = PXCFaceConfiguration::TrackingModeType::FACE_MODE_COLOR_PLUS_DEPTH;
	map[1] = PXCFaceConfiguration::TrackingModeType::FACE_MODE_COLOR;
	return map;

}

std::map<int, PXCFaceConfiguration::TrackingModeType> s_profilesMap = CreateProfileMap();

std::map<int, PXCCapture::DeviceInfo> CreateDeviceInfoMap()
{
	std::map<int, PXCCapture::DeviceInfo> map;	
	return map;
}

std::map<int, PXCCapture::DeviceInfo> g_deviceInfoMap = CreateDeviceInfoMap();

pxcCHAR* GetStringFromFaceMode(PXCFaceConfiguration::TrackingModeType mode) {

	switch (mode) {

	case PXCFaceConfiguration::TrackingModeType::FACE_MODE_COLOR_STILL:
		return L"2D_STILL";

	case PXCFaceConfiguration::TrackingModeType::FACE_MODE_COLOR:
		return L"2D";

	case PXCFaceConfiguration::TrackingModeType::FACE_MODE_COLOR_PLUS_DEPTH:
		return L"3D";

	}

	return L"";
}

void GetPlaybackFile(void) {

	OPENFILENAME filename;
	memset(&filename, 0, sizeof(filename));

	filename.lStructSize = sizeof(filename);
	filename.lpstrFilter = L"RSSDK clip (*.rssdk)\0*.rssdk\0Old format clip (*.pcsdk)\0*.pcsdk\0All Files (*.*)\0*.*\0\0";
	filename.lpstrFile = rssdkFileName; 
	rssdkFileName[0] = 0;
	filename.nMaxFile = sizeof(rssdkFileName) / sizeof(pxcCHAR);
	filename.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
	if (!GetOpenFileName(&filename)) rssdkFileName[0] = 0;

}

BOOL CALLBACK LoadCalibProc(HWND hwndDlg, UINT message, WPARAM wParam, LPARAM lParam) {

    switch (message) { 

		case WM_INITDIALOG:

			{
				
				char temp[max_path_length];
				GetTempPathA(max_path_length, temp);
				strcat_s(temp, max_path_length, "*.bin");

				struct _finddata_t dirFile;
				intptr_t hFile;

				if (( hFile = _findfirst( temp, &dirFile )) != -1 ) {

					do {

						SendMessageA(GetDlgItem(hwndDlg, IDC_LIST1), LB_ADDSTRING, 0, (LPARAM)dirFile.name);

					} while ( _findnext( hFile, &dirFile ) == 0 );

					_findclose( hFile );

				}

			}

			SendMessageA(GetDlgItem(hwndDlg, IDC_LIST1), LB_SETCURSEL, 0, (LPARAM)0);
			break;

        case WM_COMMAND: 

            switch (LOWORD(wParam)) { 

            case IDOK:			
				{
					WCHAR name[max_path_length];
					WCHAR temp[max_path_length];
					int index = SendMessage(GetDlgItem(hwndDlg, IDC_LIST1), LB_GETCURSEL, (WPARAM)0 , (LPARAM)0);
					SendMessage(GetDlgItem(hwndDlg, IDC_LIST1), LB_GETTEXT, (WPARAM)index , (LPARAM)name);
					GetTempPath(max_path_length, temp);
					
					StringCbPrintf(calibFileName, sizeof(calibFileName), L"%s%s", temp, name);
					EndDialog(hwndDlg, true);
					return TRUE; 
				}

			case IDCANCEL:
			case IDCLOSE:
				calibFileName[0] = 0;
                EndDialog(hwndDlg, false); 
                return TRUE; 

        } 

		break;

    } 

    return FALSE; 

} 

bool GetCalibFile(void) {

	// use a special dialog

	return (DialogBox(ghInstance, MAKEINTRESOURCE(IDD_LOAD_CALIB), ghWnd, (DLGPROC)LoadCalibProc)) != 0;

	// sorry we can't use common dialog because it ignores directory selection
	/*
	OPENFILENAME filename;
	memset(&filename, 0, sizeof(filename));

	WCHAR temp[max_path_length];
	GetTempPath(max_path_length, temp);

	StringCbPrintf(calibFileName, max_path_length, L"%sdefault_user.bin", temp);

	filename.lpstrInitialDir = temp;
	filename.lStructSize = sizeof(filename);
	filename.lpstrFilter = L"Calibration File (*.bin)\0*.bin\0All Files (*.*)\0*.*\0\0";
	filename.lpstrFile = calibFileName; 
	filename.nMaxFile = sizeof(calibFileName) / sizeof(pxcCHAR);
	filename.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;

	if (GetOpenFileName(&filename)) return true;

	calibFileName[0] = 0;
	return false;
*/
}

bool GetSaveCalibFile(void) {

	OPENFILENAME filename;
	memset(&filename, 0, sizeof(filename));

	filename.lStructSize = sizeof(filename);
	filename.lpstrFilter = L"Calibration File (*.bin)\0*.bin\0All Files (*.*)\0*.*\0\0";
	filename.lpstrFile = calibFileName; 
	calibFileName[0] = 0;
	filename.nMaxFile = sizeof(calibFileName) / sizeof(pxcCHAR);
	filename.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;

	if (GetSaveFileName(&filename)) return true;

	calibFileName[0] = 0;
	return false;

}

void GetRecordFile(void) {

	OPENFILENAME filename;
	memset(&filename, 0, sizeof(filename));

	filename.lStructSize = sizeof(filename);
	filename.lpstrFilter = L"RSSDK clip (*.rssdk)\0*.rssdk\0All Files (*.*)\0*.*\0\0";
	filename.lpstrFile = rssdkFileName; 
	rssdkFileName[0] = 0;
	filename.nMaxFile = sizeof(rssdkFileName) / sizeof(pxcCHAR);
	filename.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;

	if (GetSaveFileName(&filename)) {

		if (filename.nFilterIndex==1 && filename.nFileExtension==0) {

			int len = std::char_traits<wchar_t>::length(rssdkFileName);
			
			if (len>1 && len<sizeof(rssdkFileName)/sizeof(pxcCHAR)-7) {

				wcscpy_s(&rssdkFileName[len], rsize_t(7), L".rssdk\0");

			}

		}

	} else rssdkFileName[0] = 0;

}

void PopulateDevice(HMENU menu) {

	DeleteMenu(menu, 0, MF_BYPOSITION);

	PXCSession::ImplDesc desc;
	memset(&desc, 0, sizeof(desc)); 

	desc.group = PXCSession::IMPL_GROUP_SENSOR;
	desc.subgroup = PXCSession::IMPL_SUBGROUP_VIDEO_CAPTURE;

	HMENU menu1 = CreatePopupMenu();
	
	for (int i = 0, k = ID_DEVICEX; ; ++i) {

		PXCSession::ImplDesc desc1;
		
		if (session->QueryImpl(&desc, i, &desc1) < PXC_STATUS_NO_ERROR) break;

		PXCCapture *capture;

		if (session->CreateImpl<PXCCapture>(&desc1, &capture) < PXC_STATUS_NO_ERROR) continue;

		for (int j = 0; ; ++j) {

			PXCCapture::DeviceInfo deviceInfo;
			if (capture->QueryDeviceInfo(j, &deviceInfo) < PXC_STATUS_NO_ERROR) break;
			AppendMenu(menu1, MF_STRING, k++, deviceInfo.name);

		}

		capture->Release();

	}

	CheckMenuRadioItem(menu1, 0, GetMenuItemCount(menu1), 0, MF_BYPOSITION);
	InsertMenu(menu, 0, MF_BYPOSITION | MF_POPUP, (UINT_PTR)menu1, L"Device");

}

void PopulateModule(HMENU menu) {

	DeleteMenu(menu, 1, MF_BYPOSITION);

	PXCSession::ImplDesc desc, desc1;
	memset(&desc, 0, sizeof(desc));
	desc.cuids[0] = PXCFaceModule::CUID;
	HMENU menu1 = CreatePopupMenu();

	for (int i = 0; ; ++i) 	{

		if (session->QueryImpl(&desc, i, &desc1) < PXC_STATUS_NO_ERROR) break;
		AppendMenu(menu1, MF_STRING, ID_MODULEX + i, desc1.friendlyName);

	}

	CheckMenuRadioItem(menu1, 0, GetMenuItemCount(menu1), 0, MF_BYPOSITION);
	InsertMenu(menu, 1, MF_BYPOSITION | MF_POPUP, (UINT_PTR)menu1, L"Module");

}

void PopulateProfile(HWND dialogWindow) {

	HMENU menu = GetMenu(dialogWindow);
	DeleteMenu(menu, 2, MF_BYPOSITION);
	HMENU menu1 = CreatePopupMenu();

	PXCSession::ImplDesc desc;
	memset(&desc, 0, sizeof(desc));

	desc.cuids[0] = PXCFaceModule::CUID;
	wcscpy_s<sizeof(desc.friendlyName) / sizeof(pxcCHAR)>(desc.friendlyName, FaceTrackingUtilities::GetCheckedModule(dialogWindow));

	PXCFaceModule *faceModule;

	if (session->CreateImpl<PXCFaceModule>(&desc, &faceModule) >= PXC_STATUS_NO_ERROR) {

		for (unsigned int i = 0; i < s_profilesMap.size(); ++i) {

			WCHAR line[256];
			swprintf_s<sizeof(line) / sizeof(WCHAR)>(line, L"%s", GetStringFromFaceMode(s_profilesMap[i]));
			AppendMenu(menu1, MF_STRING, ID_PROFILEX + i, line);

		}

		CheckMenuRadioItem(menu1, 0, GetMenuItemCount(menu1), 0, MF_BYPOSITION);

	}

	InsertMenu(menu, 2, MF_BYPOSITION | MF_POPUP, (UINT_PTR)menu1, L"Profile");

}

void SaveLayout(HWND dialogWindow) {

	GetClientRect(dialogWindow, &layout[0]);
	ClientToScreen(dialogWindow, (LPPOINT)&layout[0].left);
	ClientToScreen(dialogWindow, (LPPOINT)&layout[0].right);
	GetWindowRect(GetDlgItem(dialogWindow, IDC_PANEL), &layout[1]);
	GetWindowRect(GetDlgItem(dialogWindow, IDC_STATUS), &layout[2]);

	for (int i = 0; i < sizeof(controls) / sizeof(controls[0]); ++i)
		GetWindowRect(GetDlgItem(dialogWindow, controls[i]), &layout[3 + i]);

}

void RedoLayout(HWND dialogWindow) {

	RECT rectangle;
	GetClientRect(dialogWindow, &rectangle);

	/* Status */

	SetWindowPos(GetDlgItem(dialogWindow, IDC_STATUS), dialogWindow, 
		0,
		rectangle.bottom - (layout[2].bottom - layout[2].top),
		rectangle.right - rectangle.left,
		(layout[2].bottom - layout[2].top),
		SWP_NOZORDER);

	/* Panel */

	SetWindowPos(
		GetDlgItem(dialogWindow,IDC_PANEL), dialogWindow,
		(layout[1].left - layout[0].left),
		(layout[1].top - layout[0].top),
		rectangle.right - (layout[1].left-layout[0].left) - (layout[0].right - layout[1].right),
		rectangle.bottom - (layout[1].top - layout[0].top) - (layout[0].bottom - layout[1].bottom),
		SWP_NOZORDER);

	/* Buttons & CheckBoxes */

	for (int i = 0; i < sizeof(controls) / sizeof(controls[0]); ++i) {

		SetWindowPos(
			GetDlgItem(dialogWindow,controls[i]), dialogWindow,
			rectangle.right - (layout[0].right - layout[3 + i].left),
			(layout[3 + i].top - layout[0].top),
			(layout[3 + i].right - layout[3 + i].left),
			(layout[3 + i].bottom - layout[3 + i].top),
			SWP_NOZORDER);

	}

}

static DWORD WINAPI RenderingThread(LPVOID arg) {

	while (true) {

		// update tracking

		UpdateTracking();

		// render

		renderer->Render();

	}

}

static DWORD WINAPI ProcessingThread(LPVOID arg) {

	HWND window = (HWND)arg;
	processor->Process(window);

	isRunning = false;
	return 0;

}

void DisableUnsupportedAlgos(HWND dialogWindow, bool isDisabled) {

	if(isDisabled) {

		CheckDlgButton(dialogWindow, IDC_LANDMARK, BST_UNCHECKED);
		CheckDlgButton(dialogWindow, IDC_POSE, BST_UNCHECKED);

	} else {

		CheckDlgButton(dialogWindow, IDC_POSE, BST_CHECKED);

	}

	Button_Enable(GetDlgItem(dialogWindow, IDC_POSE), !isDisabled);

}

BOOL CALLBACK ResultsProc(HWND hwndDlg, UINT message, WPARAM wParam, LPARAM lParam) {

    switch (message) { 

	   case WM_KEYDOWN: // exit calibration

		switch (wParam) {

		case VK_ESCAPE:

			if (calib_status == 3) { // delete calibration

				delete [] calibBuffer;
				calibBuffer = NULL;
				calibBuffersize = 0;
				isLoadCalibFile = false;
				EndDialog(hwndDlg, wParam);
				PostMessage(ghWnd, WM_COMMAND, ID_STOP, 0);

			} else { // continue working

				if (!GetDlgItemText(hwndDlg, IDC_EDIT1, user_name, 40))	wsprintf(user_name, L"default_user");
				EndDialog(hwndDlg, wParam); 

			}

			break;

		}

		case WM_INITDIALOG:
			
			wsprintf(user_name, L"default_user");
			SetDlgItemText(hwndDlg, IDC_EDIT1, user_name);
			
			if (!calibBuffer) {

				Edit_Enable(GetDlgItem(hwndDlg, IDC_EDIT1), false);
				Button_Enable(GetDlgItem(hwndDlg, ID_DETAILS) , false);
				Button_Enable(GetDlgItem(hwndDlg, ID_OK) , false);
				//SetDlgItemText(hwndDlg, ID_OK, L"Close");

			} else {

				Edit_Enable(GetDlgItem(hwndDlg, IDD_CALIB_DIALOG), true);
				Button_Enable(GetDlgItem(hwndDlg, ID_DETAILS) , true);
				Button_Enable(GetDlgItem(hwndDlg, ID_OK) , true);
				//SetDlgItemText(hwndDlg, ID_OK, L"Ok");

			}

			// show calibration results

			switch (calib_status) {

			case 0:
				SetDlgItemText(hwndDlg, IDC_RESULTS, L"Calibration Status: SUCCESS");
				break;

			case 1:
				SetDlgItemText(hwndDlg, IDC_RESULTS, L"Calibration Status: FAIR");
				break;

			case 2:
				SetDlgItemText(hwndDlg, IDC_RESULTS, L"Calibration Status: POOR");
				break;

			case 3:
				SetDlgItemText(hwndDlg, IDC_RESULTS, L"Calibration Status: FAILED");
				break;

			default:
				SetDlgItemText(hwndDlg, IDC_RESULTS, L"Calibration Status: UNKNOWN");
				break;

			}

			// show dominant eye

			switch (dominant_eye) {

			case 0:
				SetDlgItemText(hwndDlg, IDC_RESULTS_EYE, L"The current dominant eye for this calibration is: Right");
				break;

			case 1:
				SetDlgItemText(hwndDlg, IDC_RESULTS_EYE, L"The current dominant eye for this calibration is: Left");
				break;

			case 2:
				SetDlgItemText(hwndDlg, IDC_RESULTS_EYE, L"The current dominant eye for this calibration is: Both");
				break;

			default:
				SetDlgItemText(hwndDlg, IDC_RESULTS_EYE, L"The current dominant eye for this calibration is: Unknown");
				break;

			}

			break;

        case WM_COMMAND: 

            switch (LOWORD(wParam)) { 

            case ID_OK: 

				if (calib_status == 3) { // delete calibration

					delete [] calibBuffer;
					calibBuffer = NULL;
					calibBuffersize = 0;
					isLoadCalibFile = false;
					EndDialog(hwndDlg, wParam);
					PostMessage(ghWnd, WM_COMMAND, ID_STOP, 0);

				} else { // continue working

					if (!GetDlgItemText(hwndDlg, IDC_EDIT1, user_name, 40))	wsprintf(user_name, L"default_user");
					EndDialog(hwndDlg, wParam); 

				}

				return TRUE; 

			case ID_REPEAT: // force repeated calibration

				delete [] calibBuffer;
				calibBuffer = NULL;
				calibBuffersize = 0;
				isLoadCalibFile = false;
                EndDialog(hwndDlg, 0);
                return TRUE; 

            case ID_DETAILS: //TODO: Implement this
				break;

			case IDOK:
			case IDCANCEL:
			case IDCLOSE:

				if (calib_status == 3) { // delete calibration

					delete [] calibBuffer;
					calibBuffer = NULL;
					calibBuffersize = 0;
					isLoadCalibFile = false;
					EndDialog(hwndDlg, wParam);
					PostMessage(ghWnd, WM_COMMAND, ID_STOP, 0);

				}

                EndDialog(hwndDlg, wParam); 
                return TRUE; 

        } 

		break;

    } 

    return FALSE; 

} 

INT_PTR CALLBACK MessageLoopThread(HWND dialogWindow, UINT message, WPARAM wParam, LPARAM lParam) { 

	HMENU menu1 = GetMenu(dialogWindow);
	HMENU menu2;
	pxcCHAR* deviceName;

	switch (message) {

	case WM_INITDIALOG:

		PopulateDevice(menu1);

		CheckDlgButton(dialogWindow, IDC_LOCATION, BST_CHECKED);
		CheckDlgButton(dialogWindow, IDC_SCALE, BST_CHECKED);
		CheckDlgButton(dialogWindow, IDC_LANDMARK, BST_CHECKED);
		deviceName = FaceTrackingUtilities::GetCheckedDevice(dialogWindow);
		
		if (wcsstr(deviceName, L"R200") == NULL && wcsstr(deviceName, L"DS4") == NULL) {

			CheckDlgButton(dialogWindow, IDC_POSE, BST_CHECKED);

		} else {

			DisableUnsupportedAlgos(dialogWindow, true);

		}

		PopulateModule(menu1);
		PopulateProfile(dialogWindow);
		SaveLayout(dialogWindow);
		
		SendDlgItemMessageA(dialogWindow, IDC_LIST1, LB_ADDSTRING, 0, (LPARAM)"From Profile");
		SendDlgItemMessageA(dialogWindow, IDC_LIST1, LB_ADDSTRING, 0, (LPARAM)"Right Eye");
		SendDlgItemMessageA(dialogWindow, IDC_LIST1, LB_ADDSTRING, 0, (LPARAM)"Left Eye");
		SendDlgItemMessageA(dialogWindow, IDC_LIST1, LB_SETCURSEL, 0, (LPARAM)0);

		return TRUE; 

	break;

	case WM_COMMAND:

		menu2 = GetSubMenu(menu1, 0);

		if (LOWORD(wParam) >= ID_DEVICEX && LOWORD(wParam) < ID_DEVICEX + GetMenuItemCount(menu2)) {

			CheckMenuRadioItem(menu2, 0, GetMenuItemCount(menu2), LOWORD(wParam) - ID_DEVICEX, MF_BYPOSITION);	
			deviceName = FaceTrackingUtilities::GetCheckedDevice(dialogWindow);
			bool disable = (wcsstr(deviceName, L"R200") != NULL || wcsstr(deviceName, L"DS4") != NULL);
			DisableUnsupportedAlgos(dialogWindow, disable);
			return TRUE;

		}

		menu2 = GetSubMenu(menu1, 1);

		if (LOWORD(wParam) >= ID_MODULEX && LOWORD(wParam) < ID_MODULEX + GetMenuItemCount(menu2)) {

			CheckMenuRadioItem(menu2, 0, GetMenuItemCount(menu2), LOWORD(wParam) - ID_MODULEX,MF_BYPOSITION);
			PopulateProfile(dialogWindow);
			return TRUE;

		}

		menu2 = GetSubMenu(menu1, 2);

		if (LOWORD(wParam) >= ID_PROFILEX && LOWORD(wParam) < ID_PROFILEX + GetMenuItemCount(menu2)) {

			CheckMenuRadioItem(menu2, 0, GetMenuItemCount(menu2), LOWORD(wParam) - ID_PROFILEX,MF_BYPOSITION);
			HWND hwndTab = GetDlgItem(dialogWindow, IDC_TAB);
			
			if(FaceTrackingUtilities::GetCheckedProfile(dialogWindow) == PXCFaceConfiguration::FACE_MODE_COLOR)	{

				renderer->SetRendererType(FaceTrackingRenderer::R2D);

			} else if(TabCtrl_GetCurSel(hwndTab) == 1) {

				renderer->SetRendererType(FaceTrackingRenderer::R3D);

			}

			return TRUE;

		}

		switch (LOWORD(wParam)) {

		case IDCANCEL:

			isStopped = true;

			if (isRunning) {

				PostMessage(dialogWindow, WM_COMMAND, IDCANCEL, 0);

			} else {

				DestroyWindow(dialogWindow); 
				PostQuitMessage(0);

			}

			return TRUE;

		case ID_CALIB_LOADED: // calibration was loaded
			if (modeWork != mode_record)
			InitCalibWindows(mode_live);
			break;

		case ID_CALIB_DONE: // calibration was completed
			
			isPaused = true;

			CloseCalibWindows();

			if (calib_status == LOAD_CALIBRATION_ERROR) {

				MessageBoxA(ghWnd, "Load calibration failed, data invalid", "Calibration Load status ", MB_OK | MB_ICONINFORMATION);
				InitCalibWindows(mode_calib);
				isPaused = false;
				break;

			}
	
			if (calib_status == 3) {

				MessageBoxA(ghWnd, "Please repeat while being in front of the camera and observing the points.",
					"Calibration Required", MB_OK | MB_ICONINFORMATION);

				calibBuffer = NULL;
				calibBuffersize = 0;
				isLoadCalibFile = false;
				PostMessage(ghWnd, WM_COMMAND, ID_STOP, 0);

			} else {

				if (DialogBox(ghInstance, MAKEINTRESOURCE(IDD_CALIB_DIALOG), ghWnd, (DLGPROC)ResultsProc)) {

					if (calibBuffer) {

						// save this data

						WCHAR temp[max_path_length];
						WCHAR buff[max_path_length];

						GetTempPath(max_path_length, temp);
						wsprintf(buff, L"%s%s.bin", temp, user_name);

						FILE* my_file = _wfopen(buff, L"wb");

						if (my_file) {

							fwrite(calibBuffer, calibBuffersize, sizeof(pxcBYTE), my_file);
							fclose(my_file);

						}

					}
				
					InitCalibWindows(mode_live);

				} else { // repeat calibration
			
					// stop calibration threat

					isPaused = false;
					isStopped = true;
					Sleep(1000);

					// start new thread

					isStopped = false;
					isRunning = true;

					InitCalibWindows(mode_calib);

					if (processor) delete processor;
					processor = new FaceTrackingProcessor(dialogWindow);
					CreateThread(0, 0, ProcessingThread, dialogWindow, 0, 0);

					return TRUE;

				}

			}
			
			isPaused = false;
			break;

		case ID_LOAD_CALIB: // load calibration file or force a new one
			isLoadCalibFile = GetCalibFile();
			Button_Enable(GetDlgItem(dialogWindow, ID_START), calibFileName[0] != 0);
			break;

		case ID_NEW_CALIB: // start new calibration

			delete [] calibBuffer;
			calibBuffer = NULL;
			calibBuffersize = 0;
			isLoadCalibFile = false;

		case ID_START:

			Button_Enable(GetDlgItem(dialogWindow, ID_LOAD_CALIB), false);
			Button_Enable(GetDlgItem(dialogWindow, ID_NEW_CALIB), false);
			Button_Enable(GetDlgItem(dialogWindow, ID_START), false);
			Button_Enable(GetDlgItem(dialogWindow, ID_STOP), true);

			dominant_eye = SendMessage(GetDlgItem(dialogWindow, IDC_LIST1), LB_GETCURSEL, (WPARAM)0 , (LPARAM)0);

			for (int i = 0;i < GetMenuItemCount(menu1); ++i)
				EnableMenuItem(menu1, i, MF_BYPOSITION | MF_GRAYED);

			DrawMenuBar(dialogWindow);
			isStopped = false;
			isRunning = true;

			// init the calibration windows

			if (isLoadCalibFile == false && calibBuffersize == 0) {

				InitCalibWindows(mode_calib);

			} else if (GetMenuState(GetMenu(dialogWindow), ID_MODE_RECORD, MF_BYCOMMAND) & MF_CHECKED) {

				InitCalibWindows(mode_record);

			} else if (GetMenuState(GetMenu(dialogWindow), ID_MODE_PLAYBACK, MF_BYCOMMAND) & MF_CHECKED) {

				InitCalibWindows(mode_playback);

			} else {

				InitCalibWindows(mode_live);

			}

			if (processor) delete processor;
			processor = new FaceTrackingProcessor(dialogWindow);
			CreateThread(0, 0, ProcessingThread, dialogWindow, 0, 0);

			Button_Enable(GetDlgItem(dialogWindow, IDC_LOCATION), false);
			Button_Enable(GetDlgItem(dialogWindow, IDC_LANDMARK), false);
			Button_Enable(GetDlgItem(dialogWindow, IDC_POSE), false);

			Sleep(0); //TODO: remove
			return TRUE;

		case ID_STOP:

			isStopped = true;
			CloseCalibWindows();

			if (isRunning) {

				PostMessage(dialogWindow, WM_COMMAND, ID_STOP, 0);
				CloseCalibWindows();

			} else {

				for (int i = 0; i < GetMenuItemCount(menu1); ++i)
					EnableMenuItem(menu1, i, MF_BYPOSITION | MF_ENABLED);

				DrawMenuBar(dialogWindow);
	
				Button_Enable(GetDlgItem(dialogWindow, ID_START), true);
				Button_Enable(GetDlgItem(dialogWindow, ID_STOP), false);
				Button_Enable(GetDlgItem(dialogWindow, IDC_LOCATION), true);
				Button_Enable(GetDlgItem(dialogWindow, IDC_LANDMARK), true);
				Button_Enable(GetDlgItem(dialogWindow, ID_LOAD_CALIB), true);
				Button_Enable(GetDlgItem(dialogWindow, ID_NEW_CALIB), true);

				deviceName = FaceTrackingUtilities::GetCheckedDevice(dialogWindow);
				bool disable = (wcsstr(deviceName, L"R200") != NULL || wcsstr(deviceName, L"DS4") != NULL);
				DisableUnsupportedAlgos(dialogWindow, disable);			

			}

			return TRUE;

		case ID_MODE_LIVE:

			CheckMenuItem(menu1, ID_MODE_LIVE, MF_CHECKED);
			CheckMenuItem(menu1, ID_MODE_PLAYBACK, MF_UNCHECKED);
			CheckMenuItem(menu1, ID_MODE_RECORD, MF_UNCHECKED);
			return TRUE;

		case ID_MODE_PLAYBACK:

			CheckMenuItem(menu1, ID_MODE_LIVE, MF_UNCHECKED);
			CheckMenuItem(menu1, ID_MODE_PLAYBACK, MF_CHECKED);
			CheckMenuItem(menu1, ID_MODE_RECORD, MF_UNCHECKED);
			GetPlaybackFile();
			return TRUE;

		case ID_MODE_RECORD:

			CheckMenuItem(menu1, ID_MODE_LIVE, MF_UNCHECKED);
			CheckMenuItem(menu1, ID_MODE_PLAYBACK, MF_UNCHECKED);
			CheckMenuItem(menu1, ID_MODE_RECORD, MF_CHECKED);
			GetRecordFile();
			return TRUE;

		} 
		break;

	case WM_SIZE:
		RedoLayout(dialogWindow);
		return TRUE;

    case WM_ACTIVATEAPP:
        isActiveApp = wParam != 0;
        break;

	case WM_NOTIFY:

		switch (((LPNMHDR)lParam)->code) {
			
		case TCN_SELCHANGE:
			{ 
				HWND hwndTab = GetDlgItem(dialogWindow, IDC_TAB);
				int iPage = TabCtrl_GetCurSel(hwndTab);
				deviceName = FaceTrackingUtilities::GetCheckedDevice(dialogWindow);
				
				if (iPage == 0) {
					renderer->SetRendererType(FaceTrackingRenderer::R2D);
				}

				if (iPage == 1 && wcsstr(deviceName, L"3D") != NULL &&
					FaceTrackingUtilities::GetCheckedProfile(dialogWindow) == PXCFaceConfiguration::FACE_MODE_COLOR_PLUS_DEPTH) {

					renderer->SetRendererType(FaceTrackingRenderer::R3D);
				}

				return TRUE;
			} 

		}
		break;

	} 

	return FALSE; 

} 

HWND CreateTabControl(HWND hWnd, HINSTANCE hInstance)
{
	if(hWnd != NULL && hInstance != NULL)
	{
		HWND hTab = GetDlgItem(hWnd, IDC_TAB);

		if(hTab != NULL)
		{
			TC_ITEM tc;
			tc.mask = TCIF_TEXT;
			tc.pszText = L"2D";
			tc.iImage = -1;
			tc.lParam = 0;
			TabCtrl_InsertItem(hTab, 0, &tc);
			
			tc.pszText = L"3D";
			TabCtrl_InsertItem(hTab, 1, &tc);
		}
		return hTab;
	}
	return NULL;
}

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, LPTSTR, int) {

	InitCommonControls();
	ghInstance = hInstance;

	session = PXCSession::CreateInstance();
	if (session == NULL) 
	{
        MessageBoxW(0, L"Failed to create an SDK session", L"Face Viewer", MB_ICONEXCLAMATION | MB_OK);
        return 1;
    }

    HWND dialogWindow = CreateDialogW(hInstance, MAKEINTRESOURCE(IDD_MAINFRAME), 0, MessageLoopThread);
    if (!dialogWindow)  
	{
        MessageBoxW(0, L"Failed to create a window", L"Face Viewer", MB_ICONEXCLAMATION | MB_OK);
        return 1;
    }
	ghWnd = dialogWindow;

	HWND statusWindow = CreateStatusWindow(WS_CHILD | WS_VISIBLE, L"", dialogWindow, IDC_STATUS);	
	if (!statusWindow) 
	{
	   MessageBoxW(0, L"Failed to create a status bar", L"Face Viewer", MB_ICONEXCLAMATION | MB_OK);
        return 1;
	}
	
	int statusWindowParts[] = {230, -1};
	SendMessage(statusWindow, SB_SETPARTS, sizeof(statusWindowParts)/sizeof(int), (LPARAM) statusWindowParts);
	SendMessage(statusWindow, SB_SETTEXT, (WPARAM)(INT) 0, (LPARAM) (LPSTR) TEXT("OK"));
	UpdateWindow(dialogWindow);

	HWND hwndTab = CreateTabControl(dialogWindow, hInstance);
	if (!hwndTab) 
	{
		MessageBoxW(0, L"Failed to create tab control", L"Face Viewer", MB_ICONEXCLAMATION | MB_OK);
		return 1;
	}

	FaceTrackingRenderer2D* renderer2D = new FaceTrackingRenderer2D(dialogWindow);
	if(renderer2D == NULL)
	{
		MessageBoxW(0, L"Failed to create 2D renderer", L"Face Viewer", MB_ICONEXCLAMATION | MB_OK);
		return 1;
	}
	FaceTrackingRenderer3D* renderer3D = new FaceTrackingRenderer3D(dialogWindow, session);
	if(renderer3D == NULL)
	{
		MessageBoxW(0, L"Failed to create 3D renderer", L"Face Viewer", MB_ICONEXCLAMATION | MB_OK);
		delete renderer2D;
		return 1;
	}
	renderer = new FaceTrackingRendererManager(renderer2D, renderer3D);
	if(renderer == NULL)
	{
		MessageBoxW(0, L"Failed to create renderer manager", L"Face Viewer", MB_ICONEXCLAMATION | MB_OK);
		delete renderer2D;
		delete renderer3D;
		return 1;
	}

	ghMutex = CreateMutex(NULL, FALSE, NULL);
	if (ghMutex == NULL) 
	{
		MessageBoxW(0, L"Failed to create mutex", L"Face Viewer", MB_ICONEXCLAMATION | MB_OK);
		delete renderer;
		return 1;
	}

	int iPage = TabCtrl_GetCurSel(hwndTab);
	if(iPage == 0)
	{
		renderer->SetRendererType(FaceTrackingRenderer::R2D);
	}
	if(iPage == 1)
	{
		renderer->SetRendererType(FaceTrackingRenderer::R3D);
	}

	CreateThread(NULL, NULL, RenderingThread, NULL, NULL, NULL);

    MSG msg;

	while (GetMessage(&msg, NULL, 0, 0)) {

		TranslateMessage(&msg);
		DispatchMessage(&msg);

		// let other threads breath

		Sleep(0);

    }

	CloseHandle(renderer->GetRenderingFinishedSignal());

	if (calibBuffer)
		delete [] calibBuffer;

	if (processor)
		delete processor;

	if (renderer)
		delete renderer;

	session->Release();
    return (int)msg.wParam;

}