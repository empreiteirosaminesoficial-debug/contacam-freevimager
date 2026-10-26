#if !defined(AFX_HOSTPORTDLG_H__8DA6632A_D989_4EC8_ABD0_9B43877EF0C3__INCLUDED_)
#define AFX_HOSTPORTDLG_H__8DA6632A_D989_4EC8_ABD0_9B43877EF0C3__INCLUDED_

#pragma once

// HostPortDlg.h : header file
//

#ifdef VIDEODEVICEDOC

#include "HostComboBox.h"

// Shared with CameraScanDlg.cpp
void SetSockAddrPort(SOCKADDR_STORAGE& addr, int nPort);
// Returns 1 for RTSP, 2 for HTTP and 0 if the protocol is not recognized
int ProbeTcpPort(const SOCKADDR_STORAGE& addr, int nAddrLen, int nPort, const CStringA& sHostForUrl);

class CDeviceTypeModePair
{
	public:
		CDeviceTypeModePair() { m_dwMode = 0U; };
		CDeviceTypeModePair(const CString& sMode, DWORD dwMode) { m_sMode = sMode;  m_dwMode = dwMode; };
		virtual ~CDeviceTypeModePair() { ; };
		CString m_sMode;
		DWORD m_dwMode;
};

/////////////////////////////////////////////////////////////////////////////
// CHostPortDlg dialog

class CHostPortDlg : public CDialog
{
// Construction
public:
	CHostPortDlg(CWnd* pParent = NULL);   // standard constructor
	virtual ~CHostPortDlg() {;};
	static void InitDeviceTypeModeArrayForCB(CArray<CDeviceTypeModePair>& a);
	static void ParseUrl(const CString& sInHost,
						int nInPort,
						int nInDeviceTypeMode,
						CString& sOutGetFrameVideoHost,
						int& nOutGetFrameVideoPort,
						CString& sOutHttpGetFrameLocation,
						int& nOutDeviceTypeMode);
	// Quiet check whether a device with the given host, port and type is already running
	static BOOL IsRunningQuiet(const CString& sHost, int nPort, int nDeviceTypeMode);
	// Opens a RTSP camera, sUrl is the full rtsp:// url when the stream path is known.
	// The login is saved for the camera. Returns FALSE if already running or on error
	static BOOL OpenNetworkCamera(const CString& sHost, int nPort, const CString& sUrl, const CString& sUser, const CString& sPass);
	static BOOL SplitStreamUrl(const CString& sInUrl,
							BOOL& bOutRtsp,
							CString& sOutUser,
							CString& sOutPass,
							CString& sOutHost,
							int& nOutPort,
							CString& sOutLocation);
	static void MakeUrl(const CString& sInGetFrameVideoHost,
						int nInGetFrameVideoPort,
						const CString& sInGetFrameLocation,
						int nInDeviceTypeMode,
						CString& sOutHost,
						int& nOutPort,
						int& nOutDeviceTypeMode);

// Returned Data
public:
	CString m_sHost;
	int m_nPort;
	int	m_nDeviceTypeMode;
	enum { IDD = IDD_HOSTPORT };

// Implementation
protected:
	int SelectionToDeviceTypeMode();
	void DeviceTypeModeToSelection(int nDeviceTypeMode);
	void EnableDisableCtrls();
	static CString MakeDevicePathName(const CString& sInHost, int nInPort, int nInDeviceTypeMode);
	static CString GetAssignedDeviceName(const CString& sInHost, int nInPort, int nInDeviceTypeMode);
	BOOL IsRunning();		// check whether for given m_sHost, m_nPort, m_nDeviceTypeMode there is already a running device
	void LoadSettings();	// load settings for given m_sHost, m_nPort, m_nDeviceTypeMode
	void SaveSettings();	// save settings for given m_sHost, m_nPort, m_nDeviceTypeMode
	void OnError();

	CHostComboBox m_HostComboBox;
	CStringArray m_Hosts;
	CDWordArray m_Ports;
	CDWordArray m_DeviceTypeModes;
	CString m_sInitialDlgTitle;

	// Generated message map functions
	//{{AFX_MSG(CHostPortDlg)
	virtual BOOL OnInitDialog();
	virtual void OnOK();
	virtual void OnCancel();
	afx_msg void OnSelchangeComboHost();
	afx_msg void OnEditchangeComboHost();
	afx_msg void OnChangeEditPort();
	afx_msg void OnSelchangeComboDeviceTypeMode();
	afx_msg void OnCheckPreferTcpforRtsp();
	afx_msg void OnCheckUdpMulticastforRtsp();
	afx_msg void OnButtonHelp();
	afx_msg void OnButtonFillFromUrl();
	afx_msg void OnButtonScanPorts();
	afx_msg void OnButtonOpenScanLan();
	afx_msg void OnButtonOpenScanDvr();
	//}}AFX_MSG
	DECLARE_MESSAGE_MAP()
};

#endif

//{{AFX_INSERT_LOCATION}}
// Microsoft Visual C++ will insert additional declarations immediately before the previous line.

#endif // !defined(AFX_HOSTPORTDLG_H__8DA6632A_D989_4EC8_ABD0_9B43877EF0C3__INCLUDED_)
