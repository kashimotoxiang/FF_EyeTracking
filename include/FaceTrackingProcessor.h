#pragma once

#include <windows.h>

class PXCSenseManager;
class PXCFaceData;

#define OUT_OF_SCREEN 2000
#define LOAD_CALIBRATION_ERROR 5
#define NO_DETECTION_FOR_LONG 20

class FaceTrackingProcessor
{
public:
	FaceTrackingProcessor(HWND window);
	void Process(HWND dialogWindow);
	void RegisterUser();
	void UnregisterUser();

private:
	HWND m_window;
	bool m_registerFlag;
	bool m_unregisterFlag;
	PXCFaceData* m_output;

	void CheckForDepthStream(PXCSenseManager* pp, HWND hwndDlg);
	void PerformRegistration();
	void PerformUnregistration();
};