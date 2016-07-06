
#include "FaceTrackingProcessor.h"

#include <assert.h>
#include <string>
#include <fstream>

#include "pxcfacemodule.h"
#include "pxcfacedata.h"
#include "pxcfaceconfiguration.h"
#include "pxcsensemanager.h"
#include "FaceTrackingUtilities.h"
#include "FaceTrackingAlertHandler.h"
#include "FaceTrackingRendererManager.h"
#include "resource.h"

extern PXCSession* session;
extern FaceTrackingRendererManager* renderer;

extern volatile bool isStopped;
extern volatile bool isActiveApp;
extern volatile bool isLoadCalibFile;

extern pxcCHAR calibFileName[1024];
extern pxcCHAR rssdkFileName[1024];
extern HANDLE ghMutex;
bool GetSaveCalibFile(void);

// save the calibration buffer for later

unsigned char* calibBuffer = NULL;
short calibBuffersize = 0;

int calib_status = 0; // PXCFaceData::GazeCalibData::CalibratoinStatus
int dominant_eye = 0; // PXCFaceData::GazeCalibData::DominantEye

FaceTrackingProcessor::FaceTrackingProcessor(HWND window) : m_window(window), m_registerFlag(false), m_unregisterFlag(false) {

	// constructor

}

void FaceTrackingProcessor::PerformRegistration()
{
	m_registerFlag = false;
	if(m_output->QueryFaceByIndex(0))
		m_output->QueryFaceByIndex(0)->QueryRecognition()->RegisterUser();
}

void FaceTrackingProcessor::PerformUnregistration()
{
	m_unregisterFlag = false;
	if(m_output->QueryFaceByIndex(0))
		m_output->QueryFaceByIndex(0)->QueryRecognition()->UnregisterUser();
}

void FaceTrackingProcessor::CheckForDepthStream(PXCSenseManager* pp, HWND hwndDlg)
{
	PXCFaceModule* faceModule = pp->QueryFace();
	if (faceModule == NULL) 
	{
		assert(faceModule);
		return;
	}
	PXCFaceConfiguration* config = faceModule->CreateActiveConfiguration();
	if (config == NULL)
	{
		assert(config);
		return;
	}

	PXCFaceConfiguration::TrackingModeType trackingMode = config->GetTrackingMode();
	config->Release();
	if (trackingMode == PXCFaceConfiguration::FACE_MODE_COLOR_PLUS_DEPTH)
	{
		PXCCapture::Device::StreamProfileSet profiles={};
		pp->QueryCaptureManager()->QueryDevice()->QueryStreamProfileSet(&profiles);
		if (!profiles.depth.imageInfo.format)
		{            
			std::wstring msg = L"Depth stream is not supported for device: ";
			msg.append(FaceTrackingUtilities::GetCheckedDevice(hwndDlg));           
			msg.append(L". \nUsing 2D tracking");
			MessageBox(hwndDlg, msg.c_str(), L"Face Tracking", MB_OK);            
		}
	}
}

void FaceTrackingProcessor::Process(HWND dialogWindow) {

	// set startup mode

	PXCSenseManager* senseManager = session->CreateSenseManager();

	if (senseManager == NULL) {

		FaceTrackingUtilities::SetStatus(dialogWindow, L"Failed to create an SDK SenseManager", statusPart);
		return;

	}

	/* Set Mode & Source */

	PXCCaptureManager* captureManager = senseManager->QueryCaptureManager();

	if (!FaceTrackingUtilities::GetPlaybackState(dialogWindow)) {

		captureManager->FilterByDeviceInfo(FaceTrackingUtilities::GetCheckedDeviceInfo(dialogWindow));

	}

	pxcStatus status = PXC_STATUS_NO_ERROR;

	if (FaceTrackingUtilities::GetRecordState(dialogWindow)) { // we are recording

		status = captureManager->SetFileName(rssdkFileName, true);

	} else if (FaceTrackingUtilities::GetPlaybackState(dialogWindow)) { // we are playing

		status = captureManager->SetFileName(rssdkFileName, false);
		senseManager->QueryCaptureManager()->SetRealtime(true);

	}

	if (status < PXC_STATUS_NO_ERROR) {

		FaceTrackingUtilities::SetStatus(dialogWindow, L"Failed to Set Record/Playback File", statusPart);
		return;

	}

	/* Set Module */

	senseManager->EnableFace();

	/* Initialize */
	
	FaceTrackingUtilities::SetStatus(dialogWindow, L"Init Started", statusPart);

	PXCFaceModule* faceModule = senseManager->QueryFace();
	
	if (faceModule == NULL) {

		assert(faceModule);
		return;

	}

	PXCFaceConfiguration* config = faceModule->CreateActiveConfiguration();
	
	if (config == NULL) {

		assert(config);
		return;

	}

	// Enable Gaze Algo

	config->QueryGaze()->isEnabled = true;

	// set dominant eye

	if (dominant_eye) {
	
		PXCFaceData::GazeCalibData::DominantEye eye = (PXCFaceData::GazeCalibData::DominantEye)(dominant_eye - 1);
		config->QueryGaze()->SetDominantEye(eye);

	}

	// set tracking mode

	config->SetTrackingMode(FaceTrackingUtilities::GetCheckedProfile(dialogWindow));
	config->ApplyChanges();

	// Load Calibration File

	bool need_calibration = true;

	if (isLoadCalibFile) {

		FILE* my_file = _wfopen(calibFileName, L"rb");

		if (my_file) {

			if (calibBuffer == NULL) {

				calibBuffersize = config->QueryGaze()->QueryCalibDataSize();
				calibBuffer = new unsigned char[calibBuffersize];

			}

			fread(calibBuffer, calibBuffersize, sizeof(pxcBYTE), my_file);
			fclose(my_file);

			pxcStatus st = config->QueryGaze()->LoadCalibration(calibBuffer, calibBuffersize);

			if (st != PXC_STATUS_NO_ERROR) {

				// get save file name

				calib_status = LOAD_CALIBRATION_ERROR;
				need_calibration = false;
				PostMessage(dialogWindow, WM_COMMAND, ID_CALIB_DONE, 0);
				return;
				
			}

		}

		isLoadCalibFile = false;
		need_calibration = false;
		PostMessage(dialogWindow, WM_COMMAND, ID_CALIB_LOADED, 0);

	} else if (calibBuffer) {

		// load existing calib stored in memory

		config->QueryGaze()->LoadCalibration(calibBuffer, calibBuffersize);
		need_calibration = false;

	}

	// init sense manager

	if (senseManager->Init() < PXC_STATUS_NO_ERROR) {

		captureManager->FilterByStreamProfiles(NULL);

		if (senseManager->Init() < PXC_STATUS_NO_ERROR) {

			FaceTrackingUtilities::SetStatus(dialogWindow, L"Init Failed", statusPart);
			PostMessage(dialogWindow, WM_COMMAND, ID_STOP, 0);
			return;

		}

	}

	PXCCapture::DeviceInfo info;
	senseManager->QueryCaptureManager()->QueryDevice()->QueryDeviceInfo(&info);

    CheckForDepthStream(senseManager, dialogWindow);
    FaceTrackingAlertHandler alertHandler(dialogWindow);

    if (FaceTrackingUtilities::GetCheckedModule(dialogWindow)) {

        config->detection.isEnabled = FaceTrackingUtilities::IsModuleSelected(dialogWindow, IDC_LOCATION);
        config->landmarks.isEnabled = FaceTrackingUtilities::IsModuleSelected(dialogWindow, IDC_LANDMARK);
        config->pose.isEnabled = FaceTrackingUtilities::IsModuleSelected(dialogWindow, IDC_POSE);
			
        config->EnableAllAlerts();
        config->SubscribeAlert(&alertHandler);

        config->ApplyChanges();

    }

    FaceTrackingUtilities::SetStatus(dialogWindow, L"Streaming", statusPart);
    m_output = faceModule->CreateOutput();

	int failed_counter = 0;

    bool isNotFirstFrame = false;
    bool isFinishedPlaying = false;
    bool activeapp = true;
    ResetEvent(renderer->GetRenderingFinishedSignal());

	renderer->SetSenseManager(senseManager);
    renderer->SetNumberOfLandmarks(config->landmarks.numLandmarks);
    renderer->SetCallback(renderer->SignalProcessor);

	// acquisition loop

    if (!isStopped) {

        while (true) {

			if (isPaused) { // allow the application to pause for user input

				Sleep(200);
				continue;

			}

            if (senseManager->AcquireFrame(true) < PXC_STATUS_NO_ERROR) {

                isFinishedPlaying = true;

            }

            if (isNotFirstFrame) {

                WaitForSingleObject(renderer->GetRenderingFinishedSignal(), INFINITE);

            } else { // enable back window

				if (need_calibration) EnableBackWindow();

			}

            if (isFinishedPlaying || isStopped) {

                if (isStopped) senseManager->ReleaseFrame();
                if (isFinishedPlaying) PostMessage(dialogWindow, WM_COMMAND, ID_STOP, 0);
                break;

            }

            m_output->Update();
			pxcI64 stamp =  m_output->QueryFrameTimestamp();
            PXCCapture::Sample* sample = senseManager->QueryFaceSample();
            isNotFirstFrame = true;

            if (sample != NULL) {

				DWORD dwWaitResult;
				dwWaitResult = WaitForSingleObject(ghMutex,	INFINITE);
				
				if (dwWaitResult == WAIT_OBJECT_0) {

					// check calibration state

					if (need_calibration) {

						// CALIBRATION FLOW

						if (m_output->QueryNumberOfDetectedFaces()) {

							PXCFaceData::Face* trackedFace = m_output->QueryFaceByIndex(0);
							PXCFaceData::GazeCalibData* gazeData = trackedFace->QueryGazeCalibration();

							if (gazeData) { 

								// gaze enabled check calibration
								PXCFaceData::GazeCalibData::CalibrationState state = trackedFace->QueryGazeCalibration()->QueryCalibrationState();

								if (state == PXCFaceData::GazeCalibData::CALIBRATION_NEW_POINT) {

									// present new point for calibration
									PXCPointI32 new_point = trackedFace->QueryGazeCalibration()->QueryCalibPoint();
									
									// set the cursor to that point

									eye_point_x = new_point.x;
									eye_point_y = new_point.y;
									SetCursorPos(OUT_OF_SCREEN, OUT_OF_SCREEN);

								} else if (state == PXCFaceData::GazeCalibData::CALIBRATION_DONE) {

									// store calib data in a file

									calibBuffersize = trackedFace->QueryGazeCalibration()->QueryCalibDataSize();
									if (calibBuffer == NULL) calibBuffer = new unsigned char[calibBuffersize];
									calib_status = trackedFace->QueryGazeCalibration()->QueryCalibData(calibBuffer);
									dominant_eye = trackedFace->QueryGazeCalibration()->QueryCalibDominantEye();

									// get save file name

									PostMessage(dialogWindow, WM_COMMAND, ID_CALIB_DONE, 0);
									need_calibration = false;

								} else  if (state == PXCFaceData::GazeCalibData::CALIBRATION_IDLE) {

									// set the cursor beyond the screen

									eye_point_x = OUT_OF_SCREEN;
									eye_point_y = OUT_OF_SCREEN;
									SetCursorPos(OUT_OF_SCREEN, OUT_OF_SCREEN);

								}

							} else { // gaze not enabled stop processing

								need_calibration = false;
								PostMessage(dialogWindow, WM_COMMAND, ID_STOP, 0);

							}

						} else {

							failed_counter++; // wait 20 frames , if no detection happens go to failed mode

							if (failed_counter > NO_DETECTION_FOR_LONG) {

								calib_status = 3; // failed
								need_calibration = false;
								PostMessage(dialogWindow, WM_COMMAND, ID_CALIB_DONE, 0);

							}

						}

					} else {

						// GAZE PROCESSING AFTER CALIBRATION IS DONE

						if (m_output->QueryNumberOfDetectedFaces()) {

							PXCFaceData::Face* trackedFace = m_output->QueryFaceByIndex(0);
							
							// get gaze point

							if (trackedFace != NULL) {

								if (trackedFace->QueryGaze()) {

									PXCFaceData::GazePoint new_point = trackedFace->QueryGaze()->QueryGazePoint();
									eye_point_x = new_point.screenPoint.x;
									eye_point_y = new_point.screenPoint.y;

								}

							}

						}

					}

					// render output

					renderer->DrawBitmap(sample);
					renderer->SetOutput(m_output);
					renderer->SignalRenderer();

					if (!ReleaseMutex(ghMutex)) {
						
						throw std::exception("Failed to release mutex");
						return;

					}

				}

            }

            senseManager->ReleaseFrame();

        }

        m_output->Release();
        FaceTrackingUtilities::SetStatus(dialogWindow, L"Stopped", statusPart);

    }

	config->Release();
	senseManager->Close(); 
	senseManager->Release();

}

void FaceTrackingProcessor::RegisterUser()
{
	m_registerFlag = true;
}

void FaceTrackingProcessor::UnregisterUser()
{
	m_unregisterFlag = true;
}