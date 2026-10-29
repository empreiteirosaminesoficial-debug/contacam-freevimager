#if !defined(AFX_CAMERASCANDLG_H__5C1E7A62_3B1D_4F0A_9A55_6D2B8E4C1F37__INCLUDED_)
#define AFX_CAMERASCANDLG_H__5C1E7A62_3B1D_4F0A_9A55_6D2B8E4C1F37__INCLUDED_

#pragma once

// CameraScanDlg.h : header file
//
// Searches the local network for IP cameras (ONVIF WS-Discovery + RTSP port scan)
// and enumerates the channels of a DVR/NVR.

#ifdef VIDEODEVICEDOC

// Posted to the main frame, lParam = CAutoScanCtx* (ownership is passed)
#define WM_THREADSAFE_AUTOSCAN_START	WM_USER + 760
#define WM_THREADSAFE_AUTOSCAN_DONE		WM_USER + 761

/////////////////////////////////////////////////////////////////////////////
// One camera (or one DVR channel) found by the scanner

class CFoundCamera
{
public:
	CFoundCamera() { m_nPort = 0; m_nMode = -1; };
	CString m_sHost;	// IP or hostname
	int m_nPort;		// RTSP port
	CString m_sUrl;		// rtsp://host[:port]/path without credentials, empty if the stream path is unknown
	CString m_sType;	// "RTSP", "ONVIF" or "DVR ch 3"
	CString m_sInfo;	// name / hardware reported by ONVIF or additional notes
	int m_nMode;		// CVideoDeviceDoc::NetworkDeviceTypeMode of the camera maker list, -1 if unknown
	CString m_sMaker;	// name of the entry of the camera maker list
};

/////////////////////////////////////////////////////////////////////////////
// The scanner reports to a sink, the sink methods are called from the scan thread

class CScanSink
{
public:
	virtual ~CScanSink() {};
	virtual void OnFound(const CFoundCamera& cam) = 0;
	virtual void OnStatus(const CString& sStatus) = 0;
	virtual BOOL IsCancelled() = 0;
	// Pause after each batch of connections, used by the background scan so that it
	// does not disturb the streams of the cameras that are already open
	virtual DWORD GetBatchDelayMs() { return 0; };
};

/////////////////////////////////////////////////////////////////////////////
// Scanner

class CCameraScanner
{
public:
	// Sweeps all local IPv4 subnets (max 1022 hosts each) and, if sStartHost is an
	// IPv4 address, also its /24 subnet starting from that address. The hosts in
	// saExcludeHosts are not reported. If sUser/sPass are given they are used
	// to find the RTSP stream path of each camera. All the RTSP ports (554, 10554, ...)
	// that are open on a host are tried
	static void ScanLan(CScanSink& sink,
						const CStringArray& saExcludeHosts,
						const CString& sUser,
						const CString& sPass,
						const CString& sStartHost = _T(""));

	// Enumerates the channels (up to 32) of a DVR/NVR. A standalone camera answers
	// every channel: it is reported once (only if bReportSingleCamera is set)
	static void ScanDvr(CScanSink& sink,
						const CString& sHost,
						int nPort,
						const CString& sUser,
						const CString& sPass,
						BOOL bReportSingleCamera = TRUE);

	// Spy: sweeps the hosts dwFirstIp..dwLastIp (host byte order). On each host the ports of aPorts are
	// tried in the given order, the next one is used when a port does not work. If nMakerMode is >= 0
	// the camera maker list entry is used, otherwise the stream path is searched (automatic mode)
	static void ScanRange(CScanSink& sink,
						  DWORD dwFirstIp,
						  DWORD dwLastIp,
						  const CArray<int, int>& aPorts,
						  int nMakerMode,
						  const CString& sMakerName,
						  const CString& sUser,
						  const CString& sPass);

	// Entry of the camera maker list that uses the given RTSP path, -1 if there is none
	static int ModeFromRtspPath(const CString& sPath, CString* psMakerName = NULL);
};

/////////////////////////////////////////////////////////////////////////////
// Background scan started after the first successful connection of a new
// network camera. Allocate with new and pass it to StartAutoScan(), the
// ownership goes to the thread and then to the main frame which receives it
// with WM_THREADSAFE_AUTOSCAN_DONE (and deletes it)

class CAutoScanCtx : public CScanSink
{
public:
	CAutoScanCtx() { m_nPort = 0; m_hNotify = NULL; m_bCancel = FALSE; };
	virtual void OnFound(const CFoundCamera& cam) { m_Found.Add(cam); };
	virtual void OnStatus(const CString& sStatus) { ; };
	virtual BOOL IsCancelled() { return m_bCancel; };
	virtual DWORD GetBatchDelayMs() { return 150; };

	CString m_sHost;	// host of the camera that has just been connected
	int m_nPort;
	CString m_sUser;
	CString m_sPass;
	HWND m_hNotify;		// window receiving WM_THREADSAFE_AUTOSCAN_DONE
	volatile BOOL m_bCancel;
	CArray<CFoundCamera, const CFoundCamera&> m_Found;
};

void StartAutoScan(CAutoScanCtx* pCtx); // deletes pCtx if the thread cannot be started

/////////////////////////////////////////////////////////////////////////////
// CCameraScanDlg dialog

class CCameraScanDlg : public CDialog
{
// Construction
public:
	CCameraScanDlg(CWnd* pParent = NULL, UINT nIDTemplate = IDD_CAMERASCAN);
	virtual ~CCameraScanDlg();

// Input
public:
	CString m_sUser;
	CString m_sPass;
	CString m_sDvrHost;
	int m_nDvrPort;
	BOOL m_bAutoScanLan;	// start scanning the network when the dialog opens
	BOOL m_bAutoScanDvr;	// start enumerating the DVR channels when the dialog opens
	BOOL m_bSpy;			// dialog IDD_SPY: sweep of an IP range with ports, maker and transport selection
	CString m_sExcludeHost;	// this host is not reported by the network scan
	CArray<CFoundCamera, const CFoundCamera&> m_Found;	// already known results

// Output
public:
	int m_nOpened;			// number of cameras opened with OK

	enum { IDD = IDD_CAMERASCAN };

// Implementation
protected:
	void StartScan(BOOL bLan, BOOL bDvr);
	void InitSpy();
	void StartSpyScan();
	void SaveSpySettings();
	int m_nSpyTransport;	// -1 not used, 0 UDP, 1 TCP
	void StopScan();
	void AddItem(const CFoundCamera& cam);
	void EnableDisableCtrls();

	CWinThread* m_pScanThread;
	volatile LONG m_lCancel;
	BOOL m_bScanning;
	CArray<CFoundCamera, const CFoundCamera&> m_Items;

	virtual BOOL OnInitDialog();
	virtual void OnOK();
	virtual void OnCancel();
	afx_msg void OnButtonScanLan();
	afx_msg void OnButtonScanDvr();
	afx_msg void OnButtonScanStop();
	afx_msg LRESULT OnScanFound(WPARAM wparam, LPARAM lparam);
	afx_msg LRESULT OnScanStatus(WPARAM wparam, LPARAM lparam);
	afx_msg LRESULT OnScanDone(WPARAM wparam, LPARAM lparam);
	DECLARE_MESSAGE_MAP()
};

#endif

//{{AFX_INSERT_LOCATION}}
// Microsoft Visual C++ will insert additional declarations immediately before the previous line.

#endif // !defined(AFX_CAMERASCANDLG_H__5C1E7A62_3B1D_4F0A_9A55_6D2B8E4C1F37__INCLUDED_)
