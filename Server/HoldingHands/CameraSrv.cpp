#include "stdafx.h"
#include "CameraSrv.h"
#include "json\json.h"
#include "utils.h"

CCameraSrv::CCameraSrv(CClient*pClient) :
	CEventHandler(pClient,CAMERA),
	m_pCodec(NULL),
	m_pCodecContext(NULL),
	m_hBmp(NULL),
	m_hMemDC(NULL),
	m_Buffer(NULL)
{
	memset(&m_Bmp, 0, sizeof(m_Bmp));
	memset(&m_AVPacket, 0, sizeof(m_AVPacket));
	memset(&m_AVFrame, 0, sizeof(m_AVFrame));

	m_hMutex = CreateEvent(0, FALSE, TRUE, NULL);
}


CCameraSrv::~CCameraSrv()
{
	if (m_hMutex){
		CloseHandle(m_hMutex);
		m_hMutex = NULL;
	}

	//
	dbg_log("CCameraSrv::~CCameraSrv()");
}

void CCameraSrv::OnOpen()
{

}

void CCameraSrv::OnClose()
{
	CameraTerm();
	Notify(WM_CAMERA_ERROR, (WPARAM) "Connection close....");
}


void CCameraSrv::OnEvent(UINT32 e, BYTE *lpData, UINT32 Size)
{
	switch (e)
	{
	case CAMERA_DEVICELIST:
		OnDeviceList((char*)lpData);
		break;
	case CAMERA_VIDEOSIZE:
		do{
			Json::Value res;
			if (Json::Reader().parse((char*)lpData, res)){
				int code = res["code"].asInt();
				string err = res["err"].asString();
				int width = res["width"].asInt();
				int height = res["height"].asInt();

				OnVideoSize(code, err, width, height);
			}
		} while (0); 
		//OnVideoSize((DWORD*)Buffer);
		break;
	case CAMERA_FRAME:
		OnFrame((char*)lpData, Size);
		break;
	case CAMERA_ERROR:
		OnError((char*)lpData);
		break;
	case CAMERA_STOP_OK:
		OnStopOk();
		break;
	default:
		break;
	}
}

void CCameraSrv::OnDeviceList(char*DeviceList)
{
	Notify(WM_CAMERA_DEVICELIST, (WPARAM)DeviceList, NULL);
}

void CCameraSrv::Start(const char* device, int Width, int Height)
{
	Json::Value res;
	string data;
	res["device"] = device;
	res["width"] = Width;
	res["height"] = Height;
	data = Json::FastWriter().write(res);

	Send(CAMERA_START, (char*)data.c_str(), data.length() + 1);
}

void CCameraSrv::Stop()
{
	Send(CAMERA_STOP, 0, 0);
}

void CCameraSrv::OnStopOk()
{
	CameraTerm();
	Notify(WM_CAMERA_STOP_OK);
}

int CCameraSrv::CameraInit(int width, int height)
{
	BITMAPINFO bmi = { 0 };
	HDC hDC = NULL; 
	HWND hDrawWnd = NULL; 
	CameraTerm();
	
	//Create memory dc...
	hDrawWnd = (HWND)Notify(WM_CAMERA_GET_DRAW_WND);
	hDC = GetDC(hDrawWnd);
	m_hMemDC = CreateCompatibleDC(hDC);
	ReleaseDC(hDrawWnd, hDC);

	if (m_hMemDC == NULL)
		goto Failed;

	bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bmi.bmiHeader.biWidth = width;
	bmi.bmiHeader.biHeight = height;
	bmi.bmiHeader.biPlanes = 1;
	bmi.bmiHeader.biBitCount = 32;				//4字节貌似解码快
	bmi.bmiHeader.biCompression = BI_RGB;

	//创建DIBSection
	m_hBmp = CreateDIBSection(m_hMemDC, &bmi, DIB_RGB_COLORS, &m_Buffer, 0, 0);
	if (m_hBmp == NULL || !SelectObject(m_hMemDC, m_hBmp))
		goto Failed;
	//
	GetObject(m_hBmp, sizeof(BITMAP), &m_Bmp);
	//
	//创建解码器.
	m_pCodec = avcodec_find_decoder(AV_CODEC_ID_H264);
	if (m_pCodec == NULL)
		goto Failed;
	//
	m_pCodecContext = avcodec_alloc_context3(m_pCodec);
	if (m_pCodecContext == NULL)
		goto Failed;
	//
	if (0 != avcodec_open2(m_pCodecContext, m_pCodec, 0))
		goto Failed;
	return 0;

Failed:
	CameraTerm();
	return -1;
}


void CCameraSrv::CameraTerm()
{
	if (m_hMemDC){
		DeleteDC(m_hMemDC);
		m_hMemDC = NULL;
	}
	if (m_hBmp){
		DeleteObject(m_hBmp);
		memset(&m_Bmp, 0, sizeof(m_Bmp));
		m_hBmp = NULL;
	}
	if (m_pCodecContext){
		avcodec_free_context(&m_pCodecContext);
		m_pCodecContext = 0;
	}

	m_pCodec = 0;
	//AVFrame需要清除
	av_frame_unref(&m_AVFrame);

	memset(&m_AVPacket, 0, sizeof(m_AVPacket));
	memset(&m_AVFrame, 0, sizeof(m_AVFrame));
}

void CCameraSrv::OnVideoSize(int code, string&err, int width, int height){
	if (code)
	{
		Notify(WM_CAMERA_ERROR, (WPARAM)err.c_str());
		return;
	}

	if (CameraInit(width, height))
	{
		Notify(WM_CAMERA_ERROR, (WPARAM)"CameraSrv Init Failed!");
		return;
	}

	Notify(WM_CAMERA_VIDEOSIZE, width, height);
}

void CCameraSrv::OnFrame(char*buffer,DWORD dwLen)
{
	//如果m_pCodecContext 为NULL,一定是还有BUG
	ASSERT(m_pCodecContext);

	av_init_packet(&m_AVPacket);
	m_AVPacket.data = (uint8_t*)buffer;
	m_AVPacket.size = dwLen ;

	//Send(CAMERA_GETFRAME, 0, 0);
	if (!avcodec_send_packet(m_pCodecContext, &m_AVPacket))
	{
		if (!avcodec_receive_frame(m_pCodecContext, &m_AVFrame))
		{
			//成功.
			//I420 ---> ARGB.
			WaitForSingleObject(m_hMutex, INFINITE);

			libyuv::I420ToARGB(
				m_AVFrame.data[0], m_AVFrame.linesize[0], 
				m_AVFrame.data[1], m_AVFrame.linesize[1],
				m_AVFrame.data[2], m_AVFrame.linesize[2],
				(uint8_t*)m_Buffer, 
				m_Bmp.bmWidthBytes, 
				m_Bmp.bmWidth,
				m_Bmp.bmHeight);

			//显示到窗口上
			//Param1 : MemDC
			//Param2 : BmpInfo.

			Notify(WM_CAMERA_FRAME, (WPARAM)m_hMemDC, (LPARAM)&m_Bmp);
			SetEvent(m_hMutex);
			return;
		}
	}
	Notify(WM_CAMERA_ERROR, (WPARAM)"Decode Frame Failed!");
	return;
}

char * CCameraSrv::GetBmpFile(DWORD * lpDataSize)
{
	DWORD dwBitsSize = 0, dwBufferSize = 0;
	BITMAPINFOHEADER bi = { 0 };
	BITMAPFILEHEADER bmfHeader = { 0 };
	char * lpBuffer = NULL;
	int Result = 0;

	if (!m_Bmp.bmHeight || !m_Bmp.bmWidth)
	{
		return NULL;
	}

	bi.biSize = sizeof(BITMAPINFOHEADER);
	bi.biWidth = m_Bmp.bmWidth;
	bi.biHeight = m_Bmp.bmHeight;
	bi.biPlanes = 1;
	bi.biBitCount = 24;
	bi.biCompression = BI_RGB;

	dwBitsSize = ((m_Bmp.bmWidth * bi.biBitCount + 31) / 32) * 4 * m_Bmp.bmHeight;
		
	dwBufferSize += sizeof(BITMAPFILEHEADER);
	dwBufferSize += sizeof(BITMAPINFOHEADER);
	dwBufferSize += dwBitsSize;

	lpBuffer = new char[dwBufferSize];

	bmfHeader.bfType = 0x4D42;
	bmfHeader.bfOffBits = (DWORD)sizeof(BITMAPFILEHEADER) + (DWORD)sizeof(BITMAPINFOHEADER);
	bmfHeader.bfSize = dwBufferSize;

	memcpy(lpBuffer, &bmfHeader, sizeof(bmfHeader));
	memcpy(lpBuffer + sizeof(bmfHeader), &bi, sizeof(bi));

	WaitForSingleObject(m_hMutex, INFINITE);		//lock
	//get bits 
	Result = GetDIBits(
		m_hMemDC, 
		m_hBmp,
		0,
		m_Bmp.bmHeight, 
		lpBuffer + sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER), 
		(BITMAPINFO*)&bi,
		DIB_RGB_COLORS);

	SetEvent(m_hMutex);			//unlock

	if (Result != m_Bmp.bmHeight)
	{
		delete[] lpBuffer;
		return NULL;
	}

	*lpDataSize = dwBufferSize;
	return lpBuffer;
}

void CCameraSrv::OnError(char*szError)
{
	Notify(WM_CAMERA_ERROR, (WPARAM)szError, 0);
}