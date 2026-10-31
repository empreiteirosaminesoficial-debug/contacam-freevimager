// CameraScanDlg.cpp : implementation file
//
// Searches the local network for IP cameras and enumerates the channels of a DVR/NVR:
// - ONVIF WS-Discovery (UDP multicast 239.255.255.250:3702), the same discovery
//   used by the ONVIF Device Manager, returns name and hardware of the cameras
// - TCP scan of the common RTSP ports of all hosts of the local IPv4 subnets
// - RTSP DESCRIBE (with Basic and Digest authentication) to find the stream path

#include "stdafx.h"
#include "uimager.h"
#include "CameraScanDlg.h"
#include "HostPortDlg.h"
#include "VideoDeviceDoc.h"
#include "CMD5.h"
#include <iphlpapi.h>

#ifdef _DEBUG
#define new DEBUG_NEW
#undef THIS_FILE
static char THIS_FILE[] = __FILE__;
#endif

#ifdef VIDEODEVICEDOC

#pragma comment(lib, "iphlpapi.lib")

#define WM_SCAN_FOUND		(WM_USER + 770)	// lParam = CFoundCamera* (the receiver deletes it)
#define WM_SCAN_STATUS		(WM_USER + 771)	// lParam = CString* (the receiver deletes it)
#define WM_SCAN_DONE		(WM_USER + 772)

// Ports scanned on each host of the network. The first LAN_RTSP_PORTS_COUNT ports are
// RTSP ports (if a camera does not answer on 554 the next ones are tried), the last two
// are the HTTP and the stream port of the ESP32-CAM (esp32-cam-webserver).
// The open ports are stored in a DWORD mask (the ONVIF discovery finds the cameras
// that are not listening on one of these)
static const int LAN_PORTS[] = { 554, 8554, 10554, 5543, 5544, 555, 7070, 88, 8000, 8899, 80, 81 };
static const int LAN_PORTS_COUNT = sizeof(LAN_PORTS) / sizeof(LAN_PORTS[0]);
#define LAN_RTSP_PORTS_COUNT	10
#define LAN_PORT_IDX_HTTP		10
#define LAN_PORT_IDX_STREAM		11

// Stream paths that are tried to find out the RTSP url of a camera
static const LPCTSTR RTSP_PATHS[] =
{
	_T("/11"),
	_T("/onvif1"),
	_T("/live/ch0"),
	_T("/1"),			// Yoosee/Gwell GWIPC
	_T("/onvif2"),		// Yoosee/Gwell sub stream
	_T("/Streaming/Channels/101"),
	_T("/cam/realmonitor?channel=1&subtype=0"),
	_T("/h264Preview_01_main"),
	_T("/unicast/c1/s0/live"),
	_T("/live/ch00_0"),
	_T("/stream1"),
	_T("/stream"),
	_T("/video1"),
	_T("/live"),
	_T("/h264"),
	_T("/videoMain"),
	_T("/cam1/h264"),
	_T("/live1.sdp"),
	_T("/profile1/media.smp"),
	_T("/MediaInput/h264"),
	_T("/axis-media/media.amp"),
	_T("/ch01.264"),
	// More makers (national OEMs mostly use the Dahua, Hikvision, Xiongmai and Hisilicon paths)
	_T("/user=admin_password=_channel=1_stream=0.sdp?real_stream"),	// Xiongmai / XMeye / Hisilicon OEM, empty password
	_T("/user=admin&password=&channel=1&stream=0.sdp?real_stream"),
	_T("/cam/realmonitor?channel=1&subtype=1"),	// Dahua / Intelbras / Giga / Amcrest sub stream
	_T("/h264/ch1/main/av_stream"),				// Hikvision old / Honeywell / Swann
	_T("/Streaming/Channels/102"),				// Hikvision / Ezviz sub stream
	_T("/Streaming/Channels/1"),
	_T("/Preview_01_main"),						// Reolink
	_T("/h265Preview_01_main"),					// Reolink H.265
	_T("/stream2"),								// TP-Link Tapo sub stream
	_T("/tcp/av0_0"),							// VStarcam
	_T("/live/av0"),
	_T("/av0_0"),
	_T("/live0"),								// Eufy
	_T("/live/ch1"),
	_T("/live/main"),
	_T("/live.sdp"),								// Vivotek
	_T("/media/video1"),							// Sony
	_T("/profile2/media.smp"),					// Samsung / Hanwha
	_T("/h264.sdp"),								// Arecont
	_T("/CH001.sdp"),							// GeoVision
	_T("/ipcam.sdp"),							// Edimax
	_T("/play1.sdp"),							// D-Link / TrendNet
	_T("/live2.sdp"),
	_T("/channel1"),								// Brickcom
	_T("/video.mp4"),							// Zavio
	_T("/mpeg4/media.amp"),						// Axis old
	_T("/nphMpeg4/g726-640x480"),				// Panasonic old
	_T("/PSIA/Streaming/channels/1"),			// PSIA (Hikvision OEM)
	_T("/rtsp_tunnel"),							// TVT / generic DVR
	_T("/video0"),
	_T("/mjpeg/1"),
	_T("/0"),
	_T("/main"),
	_T("/sub"),
	_T("/ch0_0.h264"),							// generic Chinese OEM
	_T("/ch1/main/av_stream"),
	_T("/cam/realmonitor?channel=1&subtype=0&unicast=true&proto=Onvif")
};
static const int RTSP_PATHS_COUNT = sizeof(RTSP_PATHS) / sizeof(RTSP_PATHS[0]);

// DVR/NVR channel paths, %d is the channel number starting from 1
struct CDvrTemplate
{
	LPCTSTR m_sPath;
	LPCTSTR m_sName;
};
static const CDvrTemplate DVR_TEMPLATES[] =
{
	{ _T("/Streaming/Channels/%d01"),				_T("Hikvision") },
	{ _T("/cam/realmonitor?channel=%d&subtype=0"),	_T("Dahua / Amcrest / Lorex") },
	{ _T("/h264Preview_%02d_main"),					_T("Reolink") },
	{ _T("/unicast/c%d/s0/live"),					_T("Uniview") },
	{ _T("/axis-media/media.amp?camera=%d"),		_T("Axis") },
	{ _T("/live/ch%02d_0"),							_T("Generic /live/chNN_0") }
};
static const int DVR_TEMPLATES_COUNT = sizeof(DVR_TEMPLATES) / sizeof(DVR_TEMPLATES[0]);
#define DVR_MAX_CHANNELS	32

// Camera maker list entries (CHostPortDlg) that are selected from the RTSP path
struct CMakerPath
{
	LPCTSTR m_sPath;
	int m_nMode;
	LPCTSTR m_sName;
};
static const CMakerPath MAKER_PATHS[] =
{
	{ _T("/11"),									CVideoDeviceDoc::GENERIC_1_RTSP,		_T("Generic /11") },
	{ _T("/onvif1"),								CVideoDeviceDoc::GENERIC_2_RTSP,		_T("Generic /onvif1") },
	{ _T("/live/ch0"),							CVideoDeviceDoc::GENERIC_3_RTSP,		_T("Generic /live/ch0") },
	{ _T("/1"),									CVideoDeviceDoc::GENERIC_7_RTSP,		_T("Generic /1 (Yoosee/Gwell)") },
	{ _T("/onvif2"),								CVideoDeviceDoc::GENERIC_8_RTSP,		_T("Generic /onvif2 (Yoosee/Gwell)") },
	{ _T("/Streaming/Channels/101"),				CVideoDeviceDoc::HIKVISION_RTSP,		_T("Hikvision") },
	{ _T("/cam/realmonitor?channel=1&subtype=0"),	CVideoDeviceDoc::AMCREST_RTSP,			_T("Amcrest / Dahua / Lorex") },
	{ _T("/h264Preview_01_main"),				CVideoDeviceDoc::REOLINK_H264_RTSP,		_T("Reolink") },
	{ _T("/live/ch00_0"),						CVideoDeviceDoc::UBIQUITI_RTSP,			_T("Ubiquiti") },
	{ _T("/stream1"),								CVideoDeviceDoc::TPLINK_TAPO_RTSP,		_T("TP-Link Tapo") },
	{ _T("/videoMain"),							CVideoDeviceDoc::FOSCAM_RTSP,			_T("Foscam") },
	{ _T("/h264"),								CVideoDeviceDoc::ACTI_RTSP,				_T("ACTi / Bosch") },
	{ _T("/live1.sdp"),							CVideoDeviceDoc::DLINK_LIVE1_RTSP,		_T("D-Link") },
	{ _T("/profile1/media.smp"),				CVideoDeviceDoc::SAMSUNG_PROFILE1_RTSP,	_T("Samsung") },
	{ _T("/MediaInput/h264"),					CVideoDeviceDoc::PANASONIC_RTSP,		_T("Panasonic") },
	{ _T("/axis-media/media.amp"),				CVideoDeviceDoc::AXIS_RTSP,				_T("Axis") },
	{ _T("/live"),								CVideoDeviceDoc::DAHUA_RTSP,			_T("Dahua / Wyze") }
};
static const int MAKER_PATHS_COUNT = sizeof(MAKER_PATHS) / sizeof(MAKER_PATHS[0]);

// Makers recognized by the stream path (the stream is opened with the generic URL mode)
struct CPathBrand
{
	LPCTSTR m_sPath;
	LPCTSTR m_sName;
};
static const CPathBrand PATH_BRANDS[] =
{
	{ _T("/user=admin_password=_channel=1_stream=0.sdp?real_stream"),	_T("Xiongmai / XMeye / Hisilicon (OEM)") },
	{ _T("/user=admin&password=&channel=1&stream=0.sdp?real_stream"),	_T("Xiongmai / XMeye / Hisilicon (OEM)") },
	{ _T("/cam/realmonitor?channel=1&subtype=1"),						_T("Dahua / Intelbras / Giga / Amcrest (sub stream)") },
	{ _T("/h264/ch1/main/av_stream"),									_T("Hikvision old / Honeywell / Swann") },
	{ _T("/Streaming/Channels/102"),									_T("Hikvision / Ezviz (sub stream)") },
	{ _T("/Streaming/Channels/1"),										_T("Hikvision / Ezviz") },
	{ _T("/Preview_01_main"),											_T("Reolink") },
	{ _T("/h265Preview_01_main"),										_T("Reolink H.265") },
	{ _T("/stream2"),													_T("TP-Link Tapo (sub stream)") },
	{ _T("/tcp/av0_0"),													_T("VStarcam") },
	{ _T("/live/av0"),													_T("Generic OEM (/live/av0)") },
	{ _T("/av0_0"),														_T("Generic OEM (/av0_0)") },
	{ _T("/live0"),														_T("Eufy") },
	{ _T("/live/ch1"),													_T("Generic OEM (/live/ch1)") },
	{ _T("/live/main"),													_T("Sercomm / Generic OEM") },
	{ _T("/live.sdp"),													_T("Vivotek") },
	{ _T("/media/video1"),												_T("Sony") },
	{ _T("/profile2/media.smp"),										_T("Samsung / Hanwha") },
	{ _T("/h264.sdp"),													_T("Arecont") },
	{ _T("/CH001.sdp"),													_T("GeoVision") },
	{ _T("/ipcam.sdp"),													_T("Edimax") },
	{ _T("/play1.sdp"),													_T("D-Link / TrendNet") },
	{ _T("/live2.sdp"),													_T("D-Link") },
	{ _T("/channel1"),													_T("Brickcom") },
	{ _T("/video.mp4"),													_T("Zavio") },
	{ _T("/mpeg4/media.amp"),											_T("Axis (old)") },
	{ _T("/PSIA/Streaming/channels/1"),									_T("PSIA (Hikvision OEM)") },
	{ _T("/rtsp_tunnel"),												_T("TVT / Generic DVR") },
	{ _T("/ch0_0.h264"),												_T("Generic OEM (ch0_0.h264)") },
	{ _T("/ch1/main/av_stream"),										_T("Generic OEM (ch1/main)") },
	{ _T("/cam/realmonitor?channel=1&subtype=0&unicast=true&proto=Onvif"),	_T("Dahua / Intelbras (ONVIF)") }
};
static const int PATH_BRANDS_COUNT = sizeof(PATH_BRANDS) / sizeof(PATH_BRANDS[0]);

int CCameraScanner::ModeFromRtspPath(const CString& sPath, CString* psMakerName)
{
	for (int i = 0; i < PATH_BRANDS_COUNT; i++)
	{
		if (sPath.CompareNoCase(PATH_BRANDS[i].m_sPath) == 0)
		{
			if (psMakerName)
				*psMakerName = PATH_BRANDS[i].m_sName;
			break;
		}
	}
	for (int i = 0; i < MAKER_PATHS_COUNT; i++)
	{
		if (sPath.CompareNoCase(MAKER_PATHS[i].m_sPath) == 0)
		{
			if (psMakerName)
				*psMakerName = MAKER_PATHS[i].m_sName;
			return MAKER_PATHS[i].m_nMode;
		}
	}
	return -1;
}

/////////////////////////////////////////////////////////////////////////////
// Helpers

static CString IpToString(DWORD dwIp)
{
	CString s;
	s.Format(_T("%u.%u.%u.%u"), (dwIp >> 24) & 0xFF, (dwIp >> 16) & 0xFF, (dwIp >> 8) & 0xFF, dwIp & 0xFF);
	return s;
}

static BOOL ResolveHost(const CString& sHost, SOCKADDR_STORAGE& addr, int& nAddrLen)
{
	memset(&addr, 0, sizeof(addr));
	if (!CNetCom::StringToAddress(sHost, _T("0"), (sockaddr*)&addr))
		return FALSE;
	nAddrLen = (addr.ss_family == AF_INET6) ? (int)sizeof(SOCKADDR_IN6) : (int)sizeof(SOCKADDR_IN);
	return TRUE;
}

static BOOL IsExcluded(const CStringArray& saExcludeHosts, const CString& sHost)
{
	for (int i = 0; i < saExcludeHosts.GetSize(); i++)
	{
		if (saExcludeHosts[i].CompareNoCase(sHost) == 0)
			return TRUE;
	}
	return FALSE;
}

// Connects with a timeout, the returned socket is in blocking mode with send and receive timeouts
static SOCKET ConnectTimeout(const SOCKADDR_STORAGE& addr, int nAddrLen, int nPort, DWORD dwTimeoutMs)
{
	SOCKET s = ::socket(addr.ss_family, SOCK_STREAM, IPPROTO_TCP);
	if (s == INVALID_SOCKET)
		return INVALID_SOCKET;
	u_long ulNonBlocking = 1;
	::ioctlsocket(s, FIONBIO, &ulNonBlocking);
	SOCKADDR_STORAGE a = addr;
	SetSockAddrPort(a, nPort);
	if (::connect(s, (sockaddr*)&a, nAddrLen) != 0)
	{
		if (::WSAGetLastError() != WSAEWOULDBLOCK)
		{
			::closesocket(s);
			return INVALID_SOCKET;
		}
		fd_set wfds;
		fd_set efds;
		FD_ZERO(&wfds);
		FD_ZERO(&efds);
		FD_SET(s, &wfds);
		FD_SET(s, &efds);
		timeval tv;
		tv.tv_sec = (long)(dwTimeoutMs / 1000);
		tv.tv_usec = (long)((dwTimeoutMs % 1000) * 1000);
		if (::select(0, NULL, &wfds, &efds, &tv) <= 0 || !FD_ISSET(s, &wfds))
		{
			::closesocket(s);
			return INVALID_SOCKET;
		}
	}
	ulNonBlocking = 0;
	::ioctlsocket(s, FIONBIO, &ulNonBlocking);
	DWORD dwIoTimeoutMs = 2000;
	::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&dwIoTimeoutMs, sizeof(dwIoTimeoutMs));
	::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&dwIoTimeoutMs, sizeof(dwIoTimeoutMs));
	return s;
}

static CStringA Base64Encode(const CStringA& s)
{
	static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	CStringA out;
	int n = s.GetLength();
	for (int i = 0; i < n; i += 3)
	{
		DWORD v = ((DWORD)(BYTE)s[i]) << 16;
		if (i + 1 < n)
			v |= ((DWORD)(BYTE)s[i + 1]) << 8;
		if (i + 2 < n)
			v |= (DWORD)(BYTE)s[i + 2];
		out += tbl[(v >> 18) & 63];
		out += tbl[(v >> 12) & 63];
		out += (i + 1 < n) ? tbl[(v >> 6) & 63] : '=';
		out += (i + 2 < n) ? tbl[v & 63] : '=';
	}
	return out;
}

static CStringA Md5Hex(CMD5& md5, const CStringA& s)
{
	CMD5Hash hash;
	md5.Hash((const BYTE*)(LPCSTR)s, (DWORD)s.GetLength(), hash);
	return CStringA(hash.Format(FALSE));
}

static CStringA GetQuotedParam(const CStringA& sLine, const char* pszName)
{
	CStringA sLower(sLine);
	sLower.MakeLower();
	CStringA sKey(pszName);
	sKey += "=\"";
	int nPos = sLower.Find(sKey);
	if (nPos < 0)
		return CStringA();
	nPos += sKey.GetLength();
	int nEnd = sLine.Find('"', nPos);
	if (nEnd < 0)
		return CStringA();
	return sLine.Mid(nPos, nEnd - nPos);
}

static void GetAuthChallenges(const CStringA& sResponse, CStringA& sDigest, CStringA& sBasic)
{
	sDigest.Empty();
	sBasic.Empty();
	CStringA sLower(sResponse);
	sLower.MakeLower();
	int nPos = 0;
	while ((nPos = sLower.Find("www-authenticate:", nPos)) >= 0)
	{
		int nEnd = sLower.Find("\r\n", nPos);
		if (nEnd < 0)
			nEnd = sLower.GetLength();
		CStringA sLine(sResponse.Mid(nPos + 17, nEnd - nPos - 17)); // original case for realm and nonce
		sLine.Trim();
		CStringA sLineLower(sLine);
		sLineLower.MakeLower();
		if (sLineLower.Left(6) == "digest")
			sDigest = sLine;
		else if (sLineLower.Left(5) == "basic")
			sBasic = sLine;
		nPos = nEnd;
	}
}

static CStringA BuildDigestAuth(const CStringA& sChallenge, const CStringA& sUser, const CStringA& sPass, const CStringA& sUri)
{
	CStringA sRealm(GetQuotedParam(sChallenge, "realm"));
	CStringA sNonce(GetQuotedParam(sChallenge, "nonce"));
	CStringA sQop(GetQuotedParam(sChallenge, "qop"));
	CStringA sOpaque(GetQuotedParam(sChallenge, "opaque"));
	CMD5 md5;
	CStringA sHa1(Md5Hex(md5, sUser + ":" + sRealm + ":" + sPass));
	CStringA sHa2(Md5Hex(md5, "DESCRIBE:" + sUri));
	CStringA sAuth;
	if (sQop.Find("auth") >= 0)
	{
		const char* pszNc = "00000001";
		const char* pszCnonce = "6f8d3c2a";
		CStringA sResponse(Md5Hex(md5, sHa1 + ":" + sNonce + ":" + pszNc + ":" + pszCnonce + ":auth:" + sHa2));
		sAuth.Format("Digest username=\"%s\", realm=\"%s\", nonce=\"%s\", uri=\"%s\", response=\"%s\", qop=auth, nc=%s, cnonce=\"%s\"",
					(LPCSTR)sUser, (LPCSTR)sRealm, (LPCSTR)sNonce, (LPCSTR)sUri, (LPCSTR)sResponse, pszNc, pszCnonce);
	}
	else
	{
		CStringA sResponse(Md5Hex(md5, sHa1 + ":" + sNonce + ":" + sHa2));
		sAuth.Format("Digest username=\"%s\", realm=\"%s\", nonce=\"%s\", uri=\"%s\", response=\"%s\"",
					(LPCSTR)sUser, (LPCSTR)sRealm, (LPCSTR)sNonce, (LPCSTR)sUri, (LPCSTR)sResponse);
	}
	if (!sOpaque.IsEmpty())
		sAuth += ", opaque=\"" + sOpaque + "\"";
	return sAuth;
}

static CStringA BuildDescribe(const CStringA& sUri, int nCSeq, const CStringA& sAuth)
{
	CStringA s;
	s.Format("DESCRIBE %s RTSP/1.0\r\nCSeq: %d\r\nAccept: application/sdp\r\nUser-Agent: ContaCam\r\n", (LPCSTR)sUri, nCSeq);
	if (!sAuth.IsEmpty())
		s += "Authorization: " + sAuth + "\r\n";
	s += "\r\n";
	return s;
}

// Returns the RTSP status code, 0 on network error or if the answer is not RTSP
static int RtspRequest(const SOCKADDR_STORAGE& addr, int nAddrLen, int nPort, const CStringA& sRequest, CStringA& sResponse)
{
	sResponse.Empty();
	SOCKET s = ConnectTimeout(addr, nAddrLen, nPort, 1500);
	if (s == INVALID_SOCKET)
		return 0;
	int nCode = 0;
	if (::send(s, (LPCSTR)sRequest, sRequest.GetLength(), 0) > 0)
	{
		char buf[1024];
		while (sResponse.GetLength() < 8192)
		{
			int nRead = ::recv(s, buf, sizeof(buf), 0);
			if (nRead <= 0)
				break;
			sResponse += CStringA(buf, nRead);
			if (sResponse.Find("\r\n\r\n") >= 0)
				break;
		}
		if (sResponse.Left(5) == "RTSP/")
		{
			int nSpace = sResponse.Find(' ');
			if (nSpace > 0)
				nCode = atoi((LPCSTR)sResponse.Mid(nSpace + 1));
		}
	}
	::closesocket(s);
	return nCode;
}

// RTSP DESCRIBE of the given path, answers to a 401 with Digest or Basic authentication
// Returns the RTSP status code (200 = stream exists), 0 on network error
static int RtspDescribe(const SOCKADDR_STORAGE& addr,
						int nAddrLen,
						const CString& sHost,
						int nPort,
						const CString& sPath,
						const CString& sUser,
						const CString& sPass)
{
	CString sUrl;
	int nDummyPort;
	int nDummyMode;
	CHostPortDlg::MakeUrl(sHost, nPort, sPath, CVideoDeviceDoc::URL_RTSP, sUrl, nDummyPort, nDummyMode);
	CStringA sUri(sUrl);
	CStringA sResponse;
	int nCode = RtspRequest(addr, nAddrLen, nPort, BuildDescribe(sUri, 1, CStringA()), sResponse);
	if (nCode != 401 || sUser.IsEmpty())
		return nCode;
	CStringA sDigest;
	CStringA sBasic;
	GetAuthChallenges(sResponse, sDigest, sBasic);
	CStringA sUserA(sUser);
	CStringA sPassA(sPass);
	CStringA sAuth;
	if (!sDigest.IsEmpty())
		sAuth = BuildDigestAuth(sDigest, sUserA, sPassA, sUri);
	else if (!sBasic.IsEmpty())
		sAuth = "Basic " + Base64Encode(sUserA + ":" + sPassA);
	else
		return nCode;
	return RtspRequest(addr, nAddrLen, nPort, BuildDescribe(sUri, 2, sAuth), sResponse);
}

// Value of the Server header answered to an RTSP OPTIONS request (brand hint), empty if none
static CString RtspServerHeader(const SOCKADDR_STORAGE& addr, int nAddrLen, const CString& sHost, int nPort)
{
	CString sUrl;
	int nDummyPort;
	int nDummyMode;
	CHostPortDlg::MakeUrl(sHost, nPort, _T(""), CVideoDeviceDoc::URL_RTSP, sUrl, nDummyPort, nDummyMode);
	CStringA sRequest;
	sRequest.Format("OPTIONS %s RTSP/1.0\r\nCSeq: 1\r\nUser-Agent: ContaCam\r\n\r\n", (LPCSTR)CStringA(sUrl));
	CStringA sResponse;
	RtspRequest(addr, nAddrLen, nPort, sRequest, sResponse);
	int nPos = sResponse.Find("\r\nServer:");
	if (nPos < 0)
		return CString();
	nPos += 9;
	int nEnd = sResponse.Find("\r\n", nPos);
	if (nEnd < 0)
		nEnd = sResponse.GetLength();
	CString sServer(sResponse.Mid(nPos, nEnd - nPos));
	sServer.Trim();
	return sServer;
}

// Returns 1 if a stream path has been found, 0 if not found, -1 if a login is required or wrong,
// 2 if the camera answers every path (the path is then a guess)
static int FindRtspPath(const SOCKADDR_STORAGE& addr,
						int nAddrLen,
						const CString& sHost,
						int nPort,
						const CString& sUser,
						const CString& sPass,
						const CString& sInfoHint,
						CScanSink& sink,
						CString& sPathOut)
{
	// Some cameras answer 200 to any path: the first path of the list would always match
	int nBogus = RtspDescribe(addr, nAddrLen, sHost, nPort, _T("/contacam-no-such-stream"), sUser, sPass);
	if (nBogus == 200)
	{
		BOOL bYoosee = (sInfoHint.Find(_T("Yoosee")) >= 0 || sInfoHint.Find(_T("Gwell")) >= 0);
		sPathOut = bYoosee ? _T("/onvif1") : _T("/11");
		return 2;
	}
	for (int i = 0; i < RTSP_PATHS_COUNT; i++)
	{
		if (sink.IsCancelled())
			return 0;
		int nCode = RtspDescribe(addr, nAddrLen, sHost, nPort, RTSP_PATHS[i], sUser, sPass);
		if (nCode == 200)
		{
			sPathOut = RTSP_PATHS[i];
			return 1;
		}
		else if (nCode == 401)
			return -1;
		else if (nCode == 0)
			return 0; // no answer
	}
	return 0;
}

// Local IPv4 addresses and netmasks in host byte order
static void GetLocalIpv4(CArray<DWORD, DWORD>& aIps, CArray<DWORD, DWORD>& aMasks)
{
	ULONG ulLen = 0;
	::GetAdaptersInfo(NULL, &ulLen);
	if (ulLen == 0)
		return;
	BYTE* pBuf = new BYTE[ulLen];
	IP_ADAPTER_INFO* pInfo = (IP_ADAPTER_INFO*)pBuf;
	if (::GetAdaptersInfo(pInfo, &ulLen) == NO_ERROR)
	{
		for (IP_ADAPTER_INFO* pAdapter = pInfo; pAdapter != NULL; pAdapter = pAdapter->Next)
		{
			for (IP_ADDR_STRING* pIp = &pAdapter->IpAddressList; pIp != NULL; pIp = pIp->Next)
			{
				DWORD dwIp = ntohl(inet_addr(pIp->IpAddress.String));
				DWORD dwMask = ntohl(inet_addr(pIp->IpMask.String));
				if (dwIp == 0 || dwIp == 0xFFFFFFFF || dwMask == 0 ||
					(dwIp >> 24) == 127 ||			// loopback
					(dwIp >> 16) == 0xA9FE)			// 169.254.x.x link local
					continue;
				aIps.Add(dwIp);
				aMasks.Add(dwMask);
			}
		}
	}
	delete [] pBuf;
}

static CString GetScopeValue(const CStringA& sXml, const char* pszScope)
{
	int nPos = sXml.Find(pszScope);
	if (nPos < 0)
		return CString();
	nPos += (int)strlen(pszScope);
	int nEnd = nPos;
	while (nEnd < sXml.GetLength() && sXml[nEnd] != ' ' && sXml[nEnd] != '<' && sXml[nEnd] != '\r' && sXml[nEnd] != '\n')
		nEnd++;
	return ::UrlDecode(CString(sXml.Mid(nPos, nEnd - nPos)));
}

// ONVIF WS-Discovery, fills mapInfo with host -> "name / hardware"
static void WsDiscovery(CScanSink& sink, const CArray<DWORD, DWORD>& aLocalIps, CMapStringToString& mapInfo)
{
	SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (s == INVALID_SOCKET)
		return;
	SOCKADDR_IN local;
	memset(&local, 0, sizeof(local));
	local.sin_family = AF_INET;
	local.sin_addr.s_addr = htonl(INADDR_ANY);
	local.sin_port = 0;
	::bind(s, (sockaddr*)&local, sizeof(local));
	int nTtl = 2;
	::setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, (const char*)&nTtl, sizeof(nTtl));

	SOCKADDR_IN dest;
	memset(&dest, 0, sizeof(dest));
	dest.sin_family = AF_INET;
	dest.sin_port = htons(3702);
	dest.sin_addr.s_addr = inet_addr("239.255.255.250");

	CStringA sProbe;
	sProbe.Format(	"<?xml version=\"1.0\" encoding=\"utf-8\"?>"
					"<e:Envelope xmlns:e=\"http://www.w3.org/2003/05/soap-envelope\""
					" xmlns:w=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\""
					" xmlns:d=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\""
					" xmlns:dn=\"http://www.onvif.org/ver10/network/wsdl\">"
					"<e:Header><w:MessageID>uuid:5f3a1c2e-7b4d-4e8a-9c1f-%012x</w:MessageID>"
					"<w:To e:mustUnderstand=\"true\">urn:schemas-xmlsoap-org:ws:2005:04:discovery</w:To>"
					"<w:Action e:mustUnderstand=\"true\">http://schemas.xmlsoap.org/ws/2005/04/discovery/Probe</w:Action>"
					"</e:Header><e:Body><d:Probe><d:Types>dn:NetworkVideoTransmitter</d:Types></d:Probe></e:Body></e:Envelope>",
					(unsigned int)::GetTickCount());

	// Send the probe through each local interface
	if (aLocalIps.GetSize() == 0)
		::sendto(s, (LPCSTR)sProbe, sProbe.GetLength(), 0, (sockaddr*)&dest, sizeof(dest));
	for (int i = 0; i < aLocalIps.GetSize(); i++)
	{
		IN_ADDR ifaddr;
		ifaddr.s_addr = htonl(aLocalIps[i]);
		::setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, (const char*)&ifaddr, sizeof(ifaddr));
		::sendto(s, (LPCSTR)sProbe, sProbe.GetLength(), 0, (sockaddr*)&dest, sizeof(dest));
	}

	// Collect the answers
	const DWORD dwTimeoutMs = 2500;
	DWORD dwStart = ::GetTickCount();
	while (!sink.IsCancelled())
	{
		DWORD dwElapsed = ::GetTickCount() - dwStart;
		if (dwElapsed >= dwTimeoutMs)
			break;
		DWORD dwRemaining = dwTimeoutMs - dwElapsed;
		fd_set rfds;
		FD_ZERO(&rfds);
		FD_SET(s, &rfds);
		timeval tv;
		tv.tv_sec = (long)(dwRemaining / 1000);
		tv.tv_usec = (long)((dwRemaining % 1000) * 1000);
		if (::select(0, &rfds, NULL, NULL, &tv) <= 0)
			break;
		char buf[16384];
		SOCKADDR_IN from;
		int nFromLen = sizeof(from);
		int nRead = ::recvfrom(s, buf, sizeof(buf) - 1, 0, (sockaddr*)&from, &nFromLen);
		if (nRead == SOCKET_ERROR)
		{
			if (::WSAGetLastError() == WSAECONNRESET)
				continue; // ICMP port unreachable of a previous probe
			break;
		}
		if (nRead <= 0)
			continue;
		CStringA sXml(buf, nRead);
		if (sXml.Find("ProbeMatch") < 0)
			continue;
		CString sHost(IpToString(ntohl(from.sin_addr.s_addr)));
		CString sName(GetScopeValue(sXml, "onvif://www.onvif.org/name/"));
		CString sHardware(GetScopeValue(sXml, "onvif://www.onvif.org/hardware/"));
		// Yoosee/Gwell cameras: the ONVIF service listens on port 5000
		// (http://ip:5000/onvif/device_service), the RTSP stream path is /onvif1
		BOOL bYoosee = FALSE;
		int nXAddrs = sXml.Find("XAddrs>");
		if (nXAddrs >= 0)
		{
			nXAddrs += 7;
			int nXAddrsEnd = sXml.Find('<', nXAddrs);
			if (nXAddrsEnd > nXAddrs && sXml.Mid(nXAddrs, nXAddrsEnd - nXAddrs).Find(":5000/onvif/device_service") >= 0)
				bYoosee = TRUE;
		}
		CString sNameLower(sName + sHardware);
		sNameLower.MakeLower();
		if (sNameLower.Find(_T("yoosee")) >= 0 || sNameLower.Find(_T("gwell")) >= 0)
			bYoosee = TRUE;
		CString sInfo(sName);
		if (!sHardware.IsEmpty())
			sInfo += (sInfo.IsEmpty() ? _T("") : _T(" / ")) + sHardware;
		if (sInfo.IsEmpty())
			sInfo = _T("Dispositivo ONVIF");
		if (bYoosee)
			sInfo = _T("Yoosee/Gwell, ") + sInfo;
		mapInfo.SetAt(sHost, sInfo);
	}
	::closesocket(s);
}


// Plain HTTP GET, returns the status code (0 on network error) and the first 8 KB of the answer
static int HttpGetSimple(const SOCKADDR_STORAGE& addr, int nAddrLen, int nPort, const CString& sHost, const CString& sPath, CStringA& sAnswer)
{
	sAnswer.Empty();
	SOCKET s = ConnectTimeout(addr, nAddrLen, nPort, 1500);
	if (s == INVALID_SOCKET)
		return 0;
	CStringA sRequest;
	sRequest.Format("GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: ContaCam\r\nConnection: close\r\n\r\n", (LPCSTR)CStringA(sPath), (LPCSTR)CStringA(sHost));
	int nCode = 0;
	if (::send(s, (LPCSTR)sRequest, sRequest.GetLength(), 0) > 0)
	{
		char buf[1024];
		while (sAnswer.GetLength() < 8192)
		{
			int nRead = ::recv(s, buf, sizeof(buf), 0);
			if (nRead <= 0)
				break;
			sAnswer += CStringA(buf, nRead);
		}
		if (sAnswer.Left(5) == "HTTP/")
		{
			int nSpace = sAnswer.Find(' ');
			if (nSpace > 0)
				nCode = atoi((LPCSTR)sAnswer.Mid(nSpace + 1));
		}
	}
	::closesocket(s);
	return nCode;
}

// ESP32-CAM running the esp32-cam-webserver firmware (or the Espressif CameraWebServer example):
// - web interface and JPEG snapshot (/capture) on port 80
// - motion jpeg stream on port 81 (the exact address is reported as stream_url by /status)
// - only one client at a time can watch the stream
static BOOL DetectEsp32Cam(const SOCKADDR_STORAGE& addr, int nAddrLen, const CString& sHost, CFoundCamera& cam)
{
	CStringA sAnswer;
	if (HttpGetSimple(addr, nAddrLen, 80, sHost, _T("/status"), sAnswer) != 200)
		return FALSE;
	int nBody = sAnswer.Find("\r\n\r\n");
	if (nBody < 0)
		return FALSE;
	CStringA sJson(sAnswer.Mid(nBody + 4));
	if (sJson.Find("\"framesize\"") < 0 ||
		(sJson.Find("\"stream_url\"") < 0 && sJson.Find("\"cam_name\"") < 0 && sJson.Find("\"xclk\"") < 0))
		return FALSE;

	// Stream port, default 81
	int nStreamPort = 81;
	CStringA sStreamUrl;
	int nPos = sJson.Find("\"stream_url\"");
	if (nPos >= 0)
	{
		int nQuote1 = sJson.Find('"', nPos + 12);
		int nQuote2 = (nQuote1 >= 0) ? sJson.Find('"', nQuote1 + 1) : -1;
		if (nQuote1 >= 0 && nQuote2 > nQuote1)
			sStreamUrl = sJson.Mid(nQuote1 + 1, nQuote2 - nQuote1 - 1);
	}
	BOOL bUrlOk = FALSE;
	if (sStreamUrl.Left(7) == "http://")
	{
		CString sSplitHost;
		CString sSplitLocation;
		CString sSplitUser;
		CString sSplitPass;
		BOOL bRtsp;
		int nSplitPort;
		if (CHostPortDlg::SplitStreamUrl(CString(sStreamUrl), bRtsp, sSplitUser, sSplitPass, sSplitHost, nSplitPort, sSplitLocation))
		{
			nStreamPort = nSplitPort;
			bUrlOk = TRUE;
		}
	}
	CString sName;
	nPos = sJson.Find("\"cam_name\"");
	if (nPos >= 0)
	{
		int nQuote1 = sJson.Find('"', nPos + 10);
		int nQuote2 = (nQuote1 >= 0) ? sJson.Find('"', nQuote1 + 1) : -1;
		if (nQuote1 >= 0 && nQuote2 > nQuote1)
			sName = CString(sJson.Mid(nQuote1 + 1, nQuote2 - nQuote1 - 1));
	}

	// The firmware serves the raw stream on the root of the stream port, the Espressif example on /stream
	CString sUrl;
	CString sStreamPath(_T("/"));
	if (!bUrlOk)
		sStreamPath = _T("/stream");
	sUrl.Format(_T("http://%s:%d%s"), sHost, nStreamPort, sStreamPath);
	cam.m_sHost = sHost;
	cam.m_nPort = nStreamPort;
	cam.m_sUrl = sUrl;
	cam.m_sType = _T("ESP32-CAM");
	cam.m_sInfo = (sName.IsEmpty() ? CString() : sName + _T(", ")) + _T("HTTP motion jpeg, um espectador por vez, snapshot: /capture");
	return TRUE;
}

/////////////////////////////////////////////////////////////////////////////
// CCameraScanner

void CCameraScanner::ScanLan(	CScanSink& sink,
								const CStringArray& saExcludeHosts,
								const CString& sUser,
								const CString& sPass,
								const CString& sStartHost)
{
	// Local subnets
	CArray<DWORD, DWORD> aLocalIps;
	CArray<DWORD, DWORD> aLocalMasks;
	GetLocalIpv4(aLocalIps, aLocalMasks);
	if (aLocalIps.GetSize() == 0)
	{
		sink.OnStatus(_T("Nenhuma conex\u00e3o de rede ativa encontrada."));
		return;
	}

	// Hosts to scan
	CArray<DWORD, DWORD> aHosts;
	CMap<DWORD, DWORD, BYTE, BYTE> mapSeen;
	int i;
	for (i = 0; i < aLocalIps.GetSize(); i++)
		mapSeen.SetAt(aLocalIps[i], 1); // do not scan ourselves
	for (i = 0; i < aLocalIps.GetSize(); i++)
	{
		DWORD dwMask = aLocalMasks[i];
		if (dwMask < 0xFFFFFC00)
			dwMask = 0xFFFFFF00; // do not scan more than 1022 hosts per subnet
		DWORD dwNet = aLocalIps[i] & dwMask;
		DWORD dwFirst = dwNet + 1;
		DWORD dwLast = (dwNet | ~dwMask) - 1;
		for (DWORD dwIp = dwFirst; dwIp <= dwLast && dwIp >= dwFirst; dwIp++)
		{
			BYTE byDummy;
			if (!mapSeen.Lookup(dwIp, byDummy))
			{
				mapSeen.SetAt(dwIp, 1);
				aHosts.Add(dwIp);
			}
		}
	}

	// IP given by the user: its /24 subnet is swept too (it may not be one of the local
	// subnets) and the sweep starts at that address, for example 192.168.1.2 finds 192.168.1.3
	DWORD dwStartIp = 0;
	if (!sStartHost.IsEmpty())
	{
		unsigned long ulAddr = ::inet_addr(CStringA(sStartHost));
		if (ulAddr != INADDR_NONE && ulAddr != 0)
		{
			dwStartIp = ntohl(ulAddr);
			DWORD dwNet = dwStartIp & 0xFFFFFF00;
			for (DWORD dwIp = dwNet + 1; dwIp < dwNet + 255; dwIp++)
			{
				BYTE byDummy;
				if (!mapSeen.Lookup(dwIp, byDummy))
				{
					mapSeen.SetAt(dwIp, 1);
					aHosts.Add(dwIp);
				}
			}
			// Rotate the list so that the sweep starts after the given address
			int nFirst = -1;
			for (i = 0; i < aHosts.GetSize() && nFirst < 0; i++)
			{
				if ((aHosts[i] & 0xFFFFFF00) == dwNet && aHosts[i] > dwStartIp)
					nFirst = i;
			}
			if (nFirst > 0)
			{
				CArray<DWORD, DWORD> aRotated;
				for (i = nFirst; i < aHosts.GetSize(); i++)
					aRotated.Add(aHosts[i]);
				for (i = 0; i < nFirst; i++)
					aRotated.Add(aHosts[i]);
				aHosts.Copy(aRotated);
			}
		}
	}

	// ONVIF discovery
	CMapStringToString mapOnvif;
	sink.OnStatus(_T("Procurando c\u00e2meras ONVIF..."));
	WsDiscovery(sink, aLocalIps, mapOnvif);
	if (sink.IsCancelled())
		return;

	// Scan the RTSP ports of all hosts in batches of 60 non blocking connections (FD_SETSIZE is 64)
	const int nHostCount = (int)aHosts.GetSize();
	const int nTotal = nHostCount * LAN_PORTS_COUNT;
	const int BATCH = 60;
	CArray<DWORD, DWORD> aOpenMask; // bit mask of the open ports of each host
	aOpenMask.SetSize(nHostCount);
	for (i = 0; i < nHostCount; i++)
		aOpenMask[i] = 0;
	for (int nStart = 0; nStart < nTotal; nStart += BATCH)
	{
		if (sink.IsCancelled())
			return;
		CString sStatus;
		sStatus.Format(_T("Varrendo a rede em busca de portas RTSP... %d%%"), nStart * 100 / nTotal);
		sink.OnStatus(sStatus);

		int nCount = (nTotal - nStart < BATCH) ? (nTotal - nStart) : BATCH;
		SOCKET sockets[BATCH];
		BOOL bPending[BATCH];
		int nPending = 0;
		int k;
		for (k = 0; k < nCount; k++)
		{
			int nHost = (nStart + k) / LAN_PORTS_COUNT;
			int nPortIdx = (nStart + k) % LAN_PORTS_COUNT;
			sockets[k] = INVALID_SOCKET;
			bPending[k] = FALSE;
			SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
			if (s == INVALID_SOCKET)
				continue;
			u_long ulNonBlocking = 1;
			::ioctlsocket(s, FIONBIO, &ulNonBlocking);
			SOCKADDR_IN a;
			memset(&a, 0, sizeof(a));
			a.sin_family = AF_INET;
			a.sin_addr.s_addr = htonl(aHosts[nHost]);
			a.sin_port = htons((u_short)LAN_PORTS[nPortIdx]);
			if (::connect(s, (sockaddr*)&a, sizeof(a)) == 0)
			{
				sockets[k] = s;
				aOpenMask[nHost] = aOpenMask[nHost] | (1 << nPortIdx);
			}
			else if (::WSAGetLastError() == WSAEWOULDBLOCK)
			{
				sockets[k] = s;
				bPending[k] = TRUE;
				nPending++;
			}
			else
				::closesocket(s);
		}
		const DWORD dwBatchTimeoutMs = 450;
		DWORD dwBatchStart = ::GetTickCount();
		while (nPending > 0)
		{
			DWORD dwElapsed = ::GetTickCount() - dwBatchStart;
			if (dwElapsed >= dwBatchTimeoutMs)
				break;
			DWORD dwRemaining = dwBatchTimeoutMs - dwElapsed;
			fd_set wfds;
			fd_set efds;
			FD_ZERO(&wfds);
			FD_ZERO(&efds);
			for (k = 0; k < nCount; k++)
			{
				if (bPending[k])
				{
					FD_SET(sockets[k], &wfds);
					FD_SET(sockets[k], &efds);
				}
			}
			timeval tv;
			tv.tv_sec = (long)(dwRemaining / 1000);
			tv.tv_usec = (long)((dwRemaining % 1000) * 1000);
			if (::select(0, NULL, &wfds, &efds, &tv) <= 0)
				break;
			for (k = 0; k < nCount; k++)
			{
				if (bPending[k])
				{
					if (FD_ISSET(sockets[k], &wfds))
					{
						int nHost = (nStart + k) / LAN_PORTS_COUNT;
						int nPortIdx = (nStart + k) % LAN_PORTS_COUNT;
						aOpenMask[nHost] = aOpenMask[nHost] | (1 << nPortIdx);
						bPending[k] = FALSE;
						nPending--;
					}
					else if (FD_ISSET(sockets[k], &efds))
					{
						bPending[k] = FALSE;
						nPending--;
					}
				}
			}
		}
		for (k = 0; k < nCount; k++)
		{
			if (sockets[k] != INVALID_SOCKET)
				::closesocket(sockets[k]);
		}
		if (sink.GetBatchDelayMs() > 0)
			::Sleep(sink.GetBatchDelayMs());
	}

	// Check which protocol the open ports speak and look for the stream path
	int nFound = 0;
	for (int nHost = 0; nHost < nHostCount; nHost++)
	{
		if (sink.IsCancelled())
			return;
		if (aOpenMask[nHost] == 0)
			continue;
		CString sHost(IpToString(aHosts[nHost]));
		if (IsExcluded(saExcludeHosts, sHost))
			continue;
		SOCKADDR_STORAGE addr;
		memset(&addr, 0, sizeof(addr));
		((SOCKADDR_IN*)&addr)->sin_family = AF_INET;
		((SOCKADDR_IN*)&addr)->sin_addr.s_addr = htonl(aHosts[nHost]);
		int nAddrLen = (int)sizeof(SOCKADDR_IN);
		CStringA sHostA(sHost);

		// ESP32-CAM: web interface on port 80 and motion jpeg stream on port 81
		if ((aOpenMask[nHost] & (1 << LAN_PORT_IDX_HTTP)) != 0 && (aOpenMask[nHost] & (1 << LAN_PORT_IDX_STREAM)) != 0)
		{
			CFoundCamera esp32cam;
			if (DetectEsp32Cam(addr, nAddrLen, sHost, esp32cam))
			{
				sink.OnFound(esp32cam);
				nFound++;
				continue;
			}
		}

		// Every open port that speaks RTSP is tried: when the stream path is not found
		// on the first one (554) the next ones (10554, 8554, ...) are tried
		CArray<int, int> aRtspPorts;
		for (int p = 0; p < LAN_RTSP_PORTS_COUNT; p++)
		{
			if ((aOpenMask[nHost] & (1 << p)) && ProbeTcpPort(addr, nAddrLen, LAN_PORTS[p], sHostA) == 1)
				aRtspPorts.Add(LAN_PORTS[p]);
		}
		if (aRtspPorts.GetSize() == 0)
			continue;

		CFoundCamera cam;
		cam.m_sHost = sHost;
		cam.m_nPort = aRtspPorts[0];
		cam.m_sType = _T("RTSP");
		CString sOnvifInfo;
		if (mapOnvif.Lookup(sHost, sOnvifInfo))
		{
			cam.m_sType = _T("RTSP + ONVIF");
			cam.m_sInfo = sOnvifInfo;
			mapOnvif.RemoveKey(sHost);
		}

		CString sStatus;
		sStatus.Format(_T("C\u00e2mera encontrada em %s, procurando o caminho do stream..."), sHost);
		sink.OnStatus(sStatus);
		CString sPath;
		int nRes = 0;
		for (int p = 0; p < aRtspPorts.GetSize(); p++)
		{
			nRes = FindRtspPath(addr, nAddrLen, sHost, aRtspPorts[p], sUser, sPass, cam.m_sInfo, sink, sPath);
			if (nRes != 0)
			{
				cam.m_nPort = aRtspPorts[p];
				break;
			}
		}
		CString sNote;
		if (nRes == 1 || nRes == 2)
		{
			int nDummyPort;
			int nDummyMode;
			CHostPortDlg::MakeUrl(sHost, cam.m_nPort, sPath, CVideoDeviceDoc::URL_RTSP, cam.m_sUrl, nDummyPort, nDummyMode);
			cam.m_nMode = ModeFromRtspPath(sPath, &cam.m_sMaker);
			sNote = _T("caminho ") + sPath;
			if (nRes == 2)
				sNote += _T(" (a c\u00e2mera responde a qualquer caminho, stream principal estimado)");
			if (!cam.m_sMaker.IsEmpty())
				sNote += _T(", fabricante: ") + cam.m_sMaker;
		}
		else if (nRes < 0)
		{
			sNote = sUser.IsEmpty() ? _T("login necess\u00e1rio") : _T("login recusado");
			if (cam.m_sInfo.Find(_T("Yoosee")) >= 0)
				sNote += _T(" (Yoosee: usu\u00e1rio admin e a senha RTSP definida no app Yoosee)");
		}
		else
			sNote = _T("caminho do stream desconhecido, escolha o fabricante da c\u00e2mera");
		if (cam.m_sInfo.IsEmpty())
		{
			CString sServer(RtspServerHeader(addr, nAddrLen, sHost, cam.m_nPort));
			if (!sServer.IsEmpty())
				cam.m_sInfo = sServer;
		}
		if (!cam.m_sInfo.IsEmpty())
			cam.m_sInfo += _T(", ");
		cam.m_sInfo += sNote;
		sink.OnFound(cam);
		nFound++;
	}

	// ONVIF cameras that do not listen on one of the scanned RTSP ports
	POSITION pos = mapOnvif.GetStartPosition();
	while (pos)
	{
		CString sHost;
		CString sInfo;
		mapOnvif.GetNextAssoc(pos, sHost, sInfo);
		if (IsExcluded(saExcludeHosts, sHost))
			continue;
		CFoundCamera cam;
		cam.m_sHost = sHost;
		cam.m_nPort = RTSP_DEFAULT_PORT;
		cam.m_sType = _T("ONVIF");
		cam.m_sInfo = sInfo + _T(", porta RTSP n\u00e3o detectada");
		if (sInfo.Find(_T("Yoosee")) >= 0)
			cam.m_sInfo += _T(" (Yoosee: ative o RTSP no app Yoosee)");
		sink.OnFound(cam);
		nFound++;
	}

	CString sDone;
	sDone.Format(_T("Varredura da rede conclu\u00edda: %d c\u00e2mera(s) encontrada(s)."), nFound);
	sink.OnStatus(sDone);
}

// Maker from the text a device gives about itself (HTTP Server header / page title, UPnP SERVER)
static CString BrandFromBanner(const CStringA& sText)
{
	static const struct { const char* pszKey; const char* pszBrand; } BRANDS[] =
	{
		{ "hikvision", "Hikvision" }, { "dnvrs-webs", "Hikvision" }, { "app-webs", "Hikvision" },
		{ "dahua", "Dahua" }, { "intelbras", "Intelbras" }, { "amcrest", "Amcrest" },
		{ "axis", "Axis" }, { "foscam", "Foscam" }, { "reolink", "Reolink" },
		{ "tp-link", "TP-Link / Tapo" }, { "tapo", "TP-Link / Tapo" }, { "vivotek", "Vivotek" },
		{ "uniview", "Uniview" }, { "yoosee", "Yoosee/Gwell" }, { "gwell", "Yoosee/Gwell" },
		{ "giga", "Giga Security" }, { "multilaser", "Multilaser" }, { "elsys", "Elsys" }, { "tecvoz", "Tecvoz" }, { "luatek", "Luatek" },
		{ "jfl", "JFL" }, { "vizzion", "Vizzion" }, { "vstarcam", "VStarcam" }, { "sricam", "Sricam" }, { "wanscam", "Wanscam" }, { "ezviz", "Ezviz" },
		{ "hipcam", "Hisilicon OEM" }, { "tvt", "TVT" }, { "mobotix", "Mobotix" }, { "lorex", "Lorex" }, { "swann", "Swann" }, { "annke", "Annke" },
		{ "sony", "Sony" }, { "panasonic", "Panasonic" }, { "samsung", "Samsung / Hanwha" }, { "hanwha", "Samsung / Hanwha" },
		{ "bosch", "Bosch" }, { "avtech", "AVTech" }, { "geovision", "GeoVision" }, { "tiandy", "Tiandy" },
		{ "uc-httpd", "Xiongmai (XMeye)" }, { "xmeye", "Xiongmai (XMeye)" }, { "jaws", "Xiongmai (XMeye)" },
		{ "goahead", "generica (GoAhead)" }, { "ip camera", "camera IP generica" },
		{ "ipcam", "camera IP generica" }, { "netcam", "camera IP generica" }, { "dvr", "DVR/NVR" }, { "nvr", "DVR/NVR" }
	};
	CStringA sLower(sText);
	sLower.MakeLower();
	for (int i = 0; i < (int)(sizeof(BRANDS) / sizeof(BRANDS[0])); i++)
	{
		if (sLower.Find(BRANDS[i].pszKey) >= 0)
			return CString(BRANDS[i].pszBrand);
	}
	return CString();
}

// Value of a header line (e.g. "server") of an HTTP / SSDP message
static CStringA GetHeaderValue(const CStringA& sMsg, const char* pszName)
{
	CStringA sLower(sMsg);
	sLower.MakeLower();
	CStringA sKey;
	sKey.Format("\r\n%s:", pszName);
	int nPos = sLower.Find(sKey);
	if (nPos < 0)
		return CStringA();
	int nStart = nPos + sKey.GetLength();
	int nEnd = sMsg.Find("\r\n", nStart);
	CStringA sValue(sMsg.Mid(nStart, (nEnd >= 0 ? nEnd : sMsg.GetLength()) - nStart));
	sValue.Trim();
	return sValue;
}

// Web page of the camera: Server header and title tell the maker when RTSP does not
static CString WebFingerprint(const SOCKADDR_STORAGE& addr, int nAddrLen, const CString& sHost, int nPort, CString& sBrand)
{
	CStringA sAnswer;
	if (HttpGetSimple(addr, nAddrLen, nPort, sHost, _T("/"), sAnswer) <= 0)
		return CString();
	CStringA sServer(GetHeaderValue(sAnswer, "server"));
	CStringA sTitle;
	CStringA sLowerAnswer(sAnswer);
	sLowerAnswer.MakeLower();
	int nT1 = sLowerAnswer.Find("<title>");
	int nT2 = (nT1 >= 0) ? sLowerAnswer.Find("</title>", nT1) : -1;
	if (nT1 >= 0 && nT2 > nT1 && nT2 - nT1 < 120)
	{
		sTitle = sAnswer.Mid(nT1 + 7, nT2 - nT1 - 7);
		sTitle.Trim();
	}
	sBrand = BrandFromBanner(sServer + " " + sTitle);
	CString sInfo;
	if (!sServer.IsEmpty())
		sInfo = _T("servidor web: ") + CString(sServer);
	if (!sTitle.IsEmpty())
		sInfo += (sInfo.IsEmpty() ? _T("") : _T(", ")) + CString(_T("p\u00e1gina: ")) + CString(sTitle);
	return sInfo;
}

// SSDP/UPnP (UDP 1900) and mDNS (UDP 5353, _rtsp._tcp and _onvif._tcp): devices of the local network
// announce themselves. mapInfo: host -> text. Only devices that look like cameras are kept.
static void UdpDiscovery(CScanSink& sink, CMapStringToString& mapInfo)
{
	CArray<DWORD, DWORD> aIps;
	CArray<DWORD, DWORD> aMasks;
	GetLocalIpv4(aIps, aMasks);
	if (aIps.GetSize() == 0)
		return;
	SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (s == INVALID_SOCKET)
		return;
	SOCKADDR_IN local;
	memset(&local, 0, sizeof(local));
	local.sin_family = AF_INET;
	local.sin_addr.s_addr = htonl(INADDR_ANY);
	::bind(s, (sockaddr*)&local, sizeof(local));
	int nTtl = 2;
	::setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, (const char*)&nTtl, sizeof(nTtl));

	static const char SSDP[] =	"M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\n"
								"MAN: \"ssdp:discover\"\r\nMX: 1\r\nST: upnp:rootdevice\r\n\r\n";
	// mDNS query, "QU" bit set so the answer comes back by unicast
	static const char MDNS_RTSP[] =		"\x00\x00\x00\x00\x00\x01\x00\x00\x00\x00\x00\x00"
										"\x05_rtsp\x04_tcp\x05local\x00\x00\x0c\x80\x01";
	static const char MDNS_ONVIF[] =	"\x00\x00\x00\x00\x00\x01\x00\x00\x00\x00\x00\x00"
										"\x06_onvif\x04_tcp\x05local\x00\x00\x0c\x80\x01";
	SOCKADDR_IN ssdp, mdns;
	memset(&ssdp, 0, sizeof(ssdp));
	ssdp.sin_family = AF_INET;
	ssdp.sin_port = htons(1900);
	ssdp.sin_addr.s_addr = inet_addr("239.255.255.250");
	mdns = ssdp;
	mdns.sin_port = htons(5353);
	mdns.sin_addr.s_addr = inet_addr("224.0.0.251");
	for (int i = 0; i < aIps.GetSize(); i++)
	{
		IN_ADDR ifc;
		ifc.s_addr = htonl(aIps[i]);
		::setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, (const char*)&ifc, sizeof(ifc));
		::sendto(s, SSDP, (int)strlen(SSDP), 0, (sockaddr*)&ssdp, sizeof(ssdp));
		::sendto(s, MDNS_RTSP, (int)sizeof(MDNS_RTSP) - 1, 0, (sockaddr*)&mdns, sizeof(mdns));
		::sendto(s, MDNS_ONVIF, (int)sizeof(MDNS_ONVIF) - 1, 0, (sockaddr*)&mdns, sizeof(mdns));
	}

	DWORD dwStart = ::GetTickCount();
	while (!sink.IsCancelled())
	{
		DWORD dwElapsed = ::GetTickCount() - dwStart;
		if (dwElapsed >= 2000)
			break;
		fd_set rfds;
		FD_ZERO(&rfds);
		FD_SET(s, &rfds);
		timeval tv;
		tv.tv_sec = 0;
		tv.tv_usec = (long)((2000 - dwElapsed) * 1000);
		if (::select(0, &rfds, NULL, NULL, &tv) <= 0)
			break;
		char buf[2048];
		SOCKADDR_IN from;
		int nFromLen = sizeof(from);
		int nRead = ::recvfrom(s, buf, sizeof(buf) - 1, 0, (sockaddr*)&from, &nFromLen);
		if (nRead <= 0)
			continue;
		CString sHost(IpToString(ntohl(from.sin_addr.s_addr)));
		CStringA sMsg(buf, nRead);
		CString sNew;
		if (sMsg.Left(5) == "HTTP/")
		{
			// UPnP: keep only what looks like a camera
			CStringA sServer(GetHeaderValue(sMsg, "server"));
			CString sBrand(BrandFromBanner(sServer));
			if (!sBrand.IsEmpty() && sBrand != _T("DVR/NVR"))
				sNew = _T("UPnP: ") + CString(sServer);
		}
		else if (nRead > 12 && (((BYTE)buf[2]) & 0x80)) // mDNS answer
		{
			// printable name of the service (e.g. "camera-sala._rtsp._tcp.local")
			CString sName;
			for (int k = 12; k < nRead && k < 120; k++)
			{
				BYTE b = (BYTE)buf[k];
				sName += (b >= 32 && b < 127) ? (TCHAR)b : (TCHAR)' ';
			}
			if (sName.Find(_T("_rtsp")) >= 0 || sName.Find(_T("_onvif")) >= 0)
				sNew = _T("mDNS (_rtsp/_onvif)");
		}
		if (!sNew.IsEmpty())
		{
			CString sOld;
			if (mapInfo.Lookup(sHost, sOld))
			{
				if (sOld.Find(sNew) < 0)
					mapInfo[sHost] = sOld + _T(", ") + sNew;
			}
			else
				mapInfo[sHost] = sNew;
		}
	}
	::closesocket(s);
}

void CCameraScanner::ScanRange(	CScanSink& sink,
								DWORD dwFirstIp,
								DWORD dwLastIp,
								const CArray<int, int>& aPorts,
								int nMakerMode,
								const CString& sMakerName,
								const CString& sUser,
								const CString& sPass)
{
	CArray<DWORD, DWORD> aHosts;
	for (DWORD dwIp = dwFirstIp; dwIp <= dwLastIp && dwIp >= dwFirstIp && aHosts.GetSize() < 1024; dwIp++)
	{
		if ((dwIp & 0xFF) != 0 && (dwIp & 0xFF) != 255) // network and broadcast addresses are skipped
			aHosts.Add(dwIp);
	}
	const int nHostCount = (int)aHosts.GetSize();
	const int nPortCount = (int)aPorts.GetSize();
	if (nHostCount == 0 || nPortCount == 0 || nPortCount > 32)
	{
		sink.OnStatus(_T("Informe o intervalo de IPs e as portas."));
		return;
	}

	// Open ports of all hosts, in batches of 60 non blocking connections (FD_SETSIZE is 64)
	const int nTotal = nHostCount * nPortCount;
	const int BATCH = 60;
	CArray<DWORD, DWORD> aOpenMask;
	aOpenMask.SetSize(nHostCount);
	int i;
	for (i = 0; i < nHostCount; i++)
		aOpenMask[i] = 0;
	for (int nStart = 0; nStart < nTotal; nStart += BATCH)
	{
		if (sink.IsCancelled())
			return;
		CString sStatus;
		sStatus.Format(_T("Varrendo portas... %d%%"), nStart * 100 / nTotal);
		sink.OnStatus(sStatus);

		int nCount = (nTotal - nStart < BATCH) ? (nTotal - nStart) : BATCH;
		SOCKET sockets[BATCH];
		BOOL bPending[BATCH];
		int nPending = 0;
		int k;
		for (k = 0; k < nCount; k++)
		{
			int nHost = (nStart + k) / nPortCount;
			int nPortIdx = (nStart + k) % nPortCount;
			sockets[k] = INVALID_SOCKET;
			bPending[k] = FALSE;
			SOCKET sk = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
			if (sk == INVALID_SOCKET)
				continue;
			u_long ulNonBlocking = 1;
			::ioctlsocket(sk, FIONBIO, &ulNonBlocking);
			SOCKADDR_IN a;
			memset(&a, 0, sizeof(a));
			a.sin_family = AF_INET;
			a.sin_addr.s_addr = htonl(aHosts[nHost]);
			a.sin_port = htons((u_short)aPorts[nPortIdx]);
			if (::connect(sk, (sockaddr*)&a, sizeof(a)) == 0)
			{
				sockets[k] = sk;
				aOpenMask[nHost] = aOpenMask[nHost] | ((DWORD)1 << nPortIdx);
			}
			else if (::WSAGetLastError() == WSAEWOULDBLOCK)
			{
				sockets[k] = sk;
				bPending[k] = TRUE;
				nPending++;
			}
			else
				::closesocket(sk);
		}
		const DWORD dwBatchTimeoutMs = 450;
		DWORD dwBatchStart = ::GetTickCount();
		while (nPending > 0)
		{
			DWORD dwElapsed = ::GetTickCount() - dwBatchStart;
			if (dwElapsed >= dwBatchTimeoutMs)
				break;
			DWORD dwRemaining = dwBatchTimeoutMs - dwElapsed;
			fd_set wfds;
			fd_set efds;
			FD_ZERO(&wfds);
			FD_ZERO(&efds);
			for (k = 0; k < nCount; k++)
			{
				if (bPending[k])
				{
					FD_SET(sockets[k], &wfds);
					FD_SET(sockets[k], &efds);
				}
			}
			timeval tv;
			tv.tv_sec = (long)(dwRemaining / 1000);
			tv.tv_usec = (long)((dwRemaining % 1000) * 1000);
			if (::select(0, NULL, &wfds, &efds, &tv) <= 0)
				break;
			for (k = 0; k < nCount; k++)
			{
				if (bPending[k])
				{
					if (FD_ISSET(sockets[k], &wfds))
					{
						int nHost = (nStart + k) / nPortCount;
						int nPortIdx = (nStart + k) % nPortCount;
						aOpenMask[nHost] = aOpenMask[nHost] | ((DWORD)1 << nPortIdx);
						bPending[k] = FALSE;
						nPending--;
					}
					else if (FD_ISSET(sockets[k], &efds))
					{
						bPending[k] = FALSE;
						nPending--;
					}
				}
			}
		}
		for (k = 0; k < nCount; k++)
		{
			if (sockets[k] != INVALID_SOCKET)
				::closesocket(sockets[k]);
		}
		if (sink.GetBatchDelayMs() > 0)
			::Sleep(sink.GetBatchDelayMs());
	}

	// Cameras announcing themselves (UPnP, mDNS)
	CMapStringToString mapNet;
	sink.OnStatus(_T("Procurando an\u00fancios UPnP e mDNS..."));
	UdpDiscovery(sink, mapNet);
	CMapStringToString mapReported;

	// On each host with open ports use the first port (in the given order) that works
	int nFound = 0;
	for (int nHost = 0; nHost < nHostCount; nHost++)
	{
		if (sink.IsCancelled())
			return;
		if (aOpenMask[nHost] == 0)
			continue;
		CString sHost(IpToString(aHosts[nHost]));
		SOCKADDR_STORAGE addr;
		memset(&addr, 0, sizeof(addr));
		((SOCKADDR_IN*)&addr)->sin_family = AF_INET;
		((SOCKADDR_IN*)&addr)->sin_addr.s_addr = htonl(aHosts[nHost]);
		int nAddrLen = (int)sizeof(SOCKADDR_IN);
		CStringA sHostA(sHost);
		for (int p = 0; p < nPortCount; p++)
		{
			if (!(aOpenMask[nHost] & ((DWORD)1 << p)))
				continue;
			int nPort = aPorts[p];
			if (ProbeTcpPort(addr, nAddrLen, nPort, sHostA) != 1)
				continue; // not RTSP, next port
			CString sStatus;
			sStatus.Format(_T("C\u00e2mera encontrada em %s:%d..."), sHost, nPort);
			sink.OnStatus(sStatus);
			CFoundCamera cam;
			cam.m_sHost = sHost;
			cam.m_nPort = nPort;
			cam.m_sType = _T("RTSP");
			if (nMakerMode >= 0)
			{
				// Maker selected: the stream path comes from the camera maker list
				cam.m_nMode = nMakerMode;
				cam.m_sMaker = sMakerName;
				cam.m_sInfo = _T("fabricante: ") + sMakerName;
				sink.OnFound(cam);
				nFound++;
				break;
			}
			CString sPath;
			int nRes = FindRtspPath(addr, nAddrLen, sHost, nPort, sUser, sPass, CString(), sink, sPath);
			if (nRes == 0)
				continue; // stream path not found on this port, try the next one
			if (nRes == 1 || nRes == 2)
			{
				int nDummyPort;
				int nDummyMode;
				CHostPortDlg::MakeUrl(sHost, nPort, sPath, CVideoDeviceDoc::URL_RTSP, cam.m_sUrl, nDummyPort, nDummyMode);
				cam.m_nMode = ModeFromRtspPath(sPath, &cam.m_sMaker);
				cam.m_sInfo = _T("caminho ") + sPath;
				if (nRes == 2)
					cam.m_sInfo += _T(" (a c\u00e2mera responde a qualquer caminho, stream principal estimado)");
				if (!cam.m_sMaker.IsEmpty())
					cam.m_sInfo += _T(", fabricante: ") + cam.m_sMaker;
			}
			else
				cam.m_sInfo = sUser.IsEmpty() ? _T("login necess\u00e1rio") : _T("login recusado");
			// Web page and announcements of the same host: maker and model
			for (int w = 0; w < nPortCount; w++)
			{
				int nWeb = aPorts[w];
				if (!(aOpenMask[nHost] & ((DWORD)1 << w)) || !(nWeb == 80 || nWeb == 81 || nWeb == 88 || nWeb == 8000 || nWeb == 8080 || nWeb == 8899))
					continue;
				CString sBrand;
				CString sWeb(WebFingerprint(addr, nAddrLen, sHost, nWeb, sBrand));
				if (!sWeb.IsEmpty())
				{
					cam.m_sInfo += _T(", ") + sWeb;
					if (cam.m_sMaker.IsEmpty() && !sBrand.IsEmpty())
					{
						cam.m_sMaker = sBrand;
						cam.m_sInfo += _T(", fabricante: ") + sBrand;
					}
					break;
				}
			}
			CString sNet;
			if (mapNet.Lookup(sHost, sNet))
				cam.m_sInfo += _T(", ") + sNet;
			mapReported[sHost] = _T("1");
			sink.OnFound(cam);
			nFound++;
			break;
		}
	}

	// Announced cameras of the range whose RTSP port was not found
	for (int nH = 0; nH < nHostCount; nH++)
	{
		CString sHost(IpToString(aHosts[nH]));
		CString sNet, sDummy;
		if (!mapNet.Lookup(sHost, sNet) || mapReported.Lookup(sHost, sDummy))
			continue;
		CFoundCamera cam;
		cam.m_sHost = sHost;
		cam.m_nPort = RTSP_DEFAULT_PORT;
		cam.m_sType = _T("UPnP/mDNS");
		cam.m_sInfo = sNet + _T(", porta RTSP n\u00e3o detectada (tente outras portas ou o fabricante)");
		sink.OnFound(cam);
		nFound++;
	}

	CString sDone;
	sDone.Format(_T("Spy conclu\u00eddo: %d c\u00e2mera(s) encontrada(s)."), nFound);
	sink.OnStatus(sDone);
}

void CCameraScanner::ScanDvr(	CScanSink& sink,
								const CString& sHost,
								int nPort,
								const CString& sUser,
								const CString& sPass,
								BOOL bReportSingleCamera)
{
	SOCKADDR_STORAGE addr;
	int nAddrLen;
	if (!ResolveHost(sHost, addr, nAddrLen))
	{
		sink.OnStatus(_T("N\u00e3o foi poss\u00edvel resolver ") + sHost);
		return;
	}

	// Find the channel path format of this DVR/NVR
	sink.OnStatus(_T("Detectando o formato de stream do DVR..."));
	int nTemplate = -1;
	int t;
	for (t = 0; t < DVR_TEMPLATES_COUNT && nTemplate < 0; t++)
	{
		if (sink.IsCancelled())
			return;
		CString sPath;
		sPath.Format(DVR_TEMPLATES[t].m_sPath, 1);
		int nCode = RtspDescribe(addr, nAddrLen, sHost, nPort, sPath, sUser, sPass);
		if (nCode == 200)
			nTemplate = t;
		else if (nCode == 401)
		{
			sink.OnStatus(sUser.IsEmpty() ?	_T("O DVR exige usu\u00e1rio e senha.") :
											_T("Login do DVR recusado: verifique usu\u00e1rio e senha."));
			return;
		}
		else if (nCode == 0 && t == 0)
		{
			CString sMsg;
			sMsg.Format(_T("Sem resposta RTSP de %s porta %d."), sHost, nPort);
			sink.OnStatus(sMsg);
			return;
		}
	}
	if (nTemplate < 0)
	{
		sink.OnStatus(_T("Nenhum formato de canal de DVR conhecido: cadastre o DVR como c\u00e2mera e escolha o fabricante."));
		return;
	}

	// A standalone camera answers every channel path: it is not a DVR/NVR and listing 32
	// channels would only repeat the same camera. The cameras of the network are found
	// by the network scan
	CString sBogusPath;
	sBogusPath.Format(DVR_TEMPLATES[nTemplate].m_sPath, 99);
	if (RtspDescribe(addr, nAddrLen, sHost, nPort, sBogusPath, sUser, sPass) == 200)
	{
		CString sMsg;
		sMsg.Format(_T("%s \u00e9 uma c\u00e2mera \u00fanica, n\u00e3o um DVR (responde a todos os canais)."), sHost);
		sink.OnStatus(sMsg);
		if (bReportSingleCamera)
		{
			CFoundCamera cam;
			cam.m_sHost = sHost;
			cam.m_nPort = nPort;
			CString sPath;
			int nRes = FindRtspPath(addr, nAddrLen, sHost, nPort, sUser, sPass, CString(), sink, sPath);
			if (nRes == 1 || nRes == 2)
			{
				int nDummyPort;
				int nDummyMode;
				CHostPortDlg::MakeUrl(sHost, nPort, sPath, CVideoDeviceDoc::URL_RTSP, cam.m_sUrl, nDummyPort, nDummyMode);
				cam.m_nMode = ModeFromRtspPath(sPath, &cam.m_sMaker);
			}
			cam.m_sType = _T("RTSP");
			cam.m_sInfo = _T("c\u00e2mera \u00fanica, caminho ") + sPath;
			sink.OnFound(cam);
		}
		return;
	}

	// Enumerate the channels, stop after 4 missing channels in a row
	int nFound = 0;
	int nMisses = 0;
	for (int nCh = 1; nCh <= DVR_MAX_CHANNELS && nMisses < 4; nCh++)
	{
		if (sink.IsCancelled())
			return;
		CString sStatus;
		sStatus.Format(_T("Verificando canal %d do DVR..."), nCh);
		sink.OnStatus(sStatus);
		CString sPath;
		sPath.Format(DVR_TEMPLATES[nTemplate].m_sPath, nCh);
		if (RtspDescribe(addr, nAddrLen, sHost, nPort, sPath, sUser, sPass) == 200)
		{
			CFoundCamera cam;
			cam.m_sHost = sHost;
			cam.m_nPort = nPort;
			int nDummyPort;
			int nDummyMode;
			CHostPortDlg::MakeUrl(sHost, nPort, sPath, CVideoDeviceDoc::URL_RTSP, cam.m_sUrl, nDummyPort, nDummyMode);
			cam.m_sType.Format(_T("Canal DVR %d"), nCh);
			cam.m_sInfo = DVR_TEMPLATES[nTemplate].m_sName;
			cam.m_nMode = ModeFromRtspPath(sPath, &cam.m_sMaker); // only channel 1 has an entry in the maker list
			if (cam.m_nMode >= 0)
				cam.m_sInfo += _T(", fabricante: ") + cam.m_sMaker;
			sink.OnFound(cam);
			nFound++;
			nMisses = 0;
		}
		else
			nMisses++;
	}
	CString sDone;
	sDone.Format(_T("Varredura do DVR conclu\u00edda: %d canal(is) encontrado(s)."), nFound);
	sink.OnStatus(sDone);
}

/////////////////////////////////////////////////////////////////////////////
// Background scan after the first successful connection

class CCollectSink : public CScanSink
{
public:
	CCollectSink(CScanSink& parent) : m_Parent(parent) { ; };
	virtual void OnFound(const CFoundCamera& cam) { m_Found.Add(cam); };
	virtual void OnStatus(const CString& sStatus) { ; };
	virtual BOOL IsCancelled() { return m_Parent.IsCancelled(); };
	virtual DWORD GetBatchDelayMs() { return m_Parent.GetBatchDelayMs(); };
	CArray<CFoundCamera, const CFoundCamera&> m_Found;
protected:
	CScanSink& m_Parent;
};

static UINT AutoScanThreadProc(LPVOID pParam)
{
	CAutoScanCtx* pCtx = (CAutoScanCtx*)pParam;

	// Other cameras in the network
	CStringArray saExclude;
	saExclude.Add(pCtx->m_sHost);
	CCameraScanner::ScanLan(*pCtx, saExclude, pCtx->m_sUser, pCtx->m_sPass, CString());

	// A DVR/NVR shows its channels on the same address,
	// report them only if there are at least two
	if (pCtx->m_nPort > 0 && !pCtx->IsCancelled())
	{
		CCollectSink DvrSink(*pCtx);
		CCameraScanner::ScanDvr(DvrSink, pCtx->m_sHost, pCtx->m_nPort, pCtx->m_sUser, pCtx->m_sPass, FALSE);
		if (DvrSink.m_Found.GetSize() >= 2)
			pCtx->m_Found.Append(DvrSink.m_Found);
	}

	// Pass the ownership to the main frame
	if (!pCtx->m_hNotify || !::PostMessage(pCtx->m_hNotify, WM_THREADSAFE_AUTOSCAN_DONE, 0, (LPARAM)pCtx))
		delete pCtx;
	return 0;
}

void StartAutoScan(CAutoScanCtx* pCtx)
{
	if (!::AfxBeginThread(AutoScanThreadProc, pCtx, THREAD_PRIORITY_BELOW_NORMAL))
		delete pCtx;
}

/////////////////////////////////////////////////////////////////////////////
// Spy after 2 minutes without connection

// Only addresses of the local network are scanned
static BOOL IsPrivateIpv4(const CString& sHost, DWORD& dwIp)
{
	unsigned long ulAddr = ::inet_addr(CStringA(sHost));
	if (ulAddr == INADDR_NONE || ulAddr == 0)
		return FALSE;
	dwIp = ntohl(ulAddr);
	int a = (int)((dwIp >> 24) & 0xFF);
	int b = (int)((dwIp >> 16) & 0xFF);
	return (a == 10 || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168) || (a == 169 && b == 254));
}

static UINT AutoFixThreadProc(LPVOID pParam)
{
	CAutoFixCtx* pCtx = (CAutoFixCtx*)pParam;
	DWORD dwIp = 0;
	if (IsPrivateIpv4(pCtx->m_sHost, dwIp))
	{
		// The port of the camera first, then the usual ones
		static const int DEFAULT_PORTS[] = { 554, 10554, 8554, 5543, 5544, 555, 7070, 88, 8000 };
		CArray<int, int> aPorts;
		if (pCtx->m_nOldPort > 0 && pCtx->m_nOldPort <= 65535)
			aPorts.Add(pCtx->m_nOldPort);
		for (int i = 0; i < (int)(sizeof(DEFAULT_PORTS) / sizeof(DEFAULT_PORTS[0])); i++)
		{
			if (DEFAULT_PORTS[i] != pCtx->m_nOldPort)
				aPorts.Add(DEFAULT_PORTS[i]);
		}
		CCameraScanner::ScanRange(*pCtx, dwIp, dwIp, aPorts, -1, CString(), pCtx->m_sUser, pCtx->m_sPass);
	}
	if (!pCtx->m_hNotify || !::PostMessage(pCtx->m_hNotify, WM_THREADSAFE_AUTOFIX_DONE, 0, (LPARAM)pCtx))
		delete pCtx;
	return 0;
}

void StartAutoFix(CAutoFixCtx* pCtx)
{
	if (!::AfxBeginThread(AutoFixThreadProc, pCtx, THREAD_PRIORITY_BELOW_NORMAL))
		delete pCtx;
}

/////////////////////////////////////////////////////////////////////////////
// Scan thread of the dialog

class CDlgScanSink : public CScanSink
{
public:
	CDlgScanSink(HWND hWnd, volatile LONG* plCancel) : m_hWnd(hWnd), m_plCancel(plCancel) { ; };
	virtual void OnFound(const CFoundCamera& cam)
	{
		CFoundCamera* p = new CFoundCamera(cam);
		if (!::PostMessage(m_hWnd, WM_SCAN_FOUND, 0, (LPARAM)p))
			delete p;
	};
	virtual void OnStatus(const CString& sStatus)
	{
		CString* p = new CString(sStatus);
		if (!::PostMessage(m_hWnd, WM_SCAN_STATUS, 0, (LPARAM)p))
			delete p;
	};
	virtual BOOL IsCancelled() { return *m_plCancel != 0; };
protected:
	HWND m_hWnd;
	volatile LONG* m_plCancel;
};

struct CScanJob
{
	BOOL m_bSpy;
	DWORD m_dwFirstIp;
	DWORD m_dwLastIp;
	CArray<int, int> m_aPorts;
	int m_nMakerMode;
	CString m_sMakerName;
	BOOL m_bLan;
	BOOL m_bDvr;
	CString m_sHost;
	int m_nPort;
	CString m_sUser;
	CString m_sPass;
	CString m_sExclude;
	HWND m_hWnd;
	volatile LONG* m_plCancel;
};

static UINT DlgScanThreadProc(LPVOID pParam)
{
	CScanJob* pJob = (CScanJob*)pParam;
	CDlgScanSink sink(pJob->m_hWnd, pJob->m_plCancel);
	if (pJob->m_bSpy)
	{
		CCameraScanner::ScanRange(sink, pJob->m_dwFirstIp, pJob->m_dwLastIp, pJob->m_aPorts, pJob->m_nMakerMode, pJob->m_sMakerName, pJob->m_sUser, pJob->m_sPass);
		::PostMessage(pJob->m_hWnd, WM_SCAN_DONE, 0, 0);
		delete pJob;
		return 0;
	}
	// DVR first (fast), then the sweep of the network
	if (pJob->m_bDvr)
		CCameraScanner::ScanDvr(sink, pJob->m_sHost, pJob->m_nPort, pJob->m_sUser, pJob->m_sPass, !pJob->m_bLan);
	if (pJob->m_bLan && !sink.IsCancelled())
	{
		CStringArray saExclude;
		if (!pJob->m_sExclude.IsEmpty())
			saExclude.Add(pJob->m_sExclude);
		CCameraScanner::ScanLan(sink, saExclude, pJob->m_sUser, pJob->m_sPass, pJob->m_sHost);
	}
	::PostMessage(pJob->m_hWnd, WM_SCAN_DONE, 0, 0);
	delete pJob;
	return 0;
}

/////////////////////////////////////////////////////////////////////////////
// CCameraScanDlg dialog

CCameraScanDlg::CCameraScanDlg(CWnd* pParent /*=NULL*/, UINT nIDTemplate /*=IDD_CAMERASCAN*/)
	: CDialog(nIDTemplate, pParent)
{
	m_bSpy = (nIDTemplate == IDD_SPY);
	m_nSpyTransport = -1;
	m_nDvrPort = RTSP_DEFAULT_PORT;
	m_bAutoScanLan = FALSE;
	m_bAutoScanDvr = FALSE;
	m_nOpened = 0;
	m_pScanThread = NULL;
	m_lCancel = 0;
	m_bScanning = FALSE;
}

CCameraScanDlg::~CCameraScanDlg()
{
	StopScan();
}

BEGIN_MESSAGE_MAP(CCameraScanDlg, CDialog)
	ON_BN_CLICKED(IDC_BUTTON_SCAN_LAN, OnButtonScanLan)
	ON_BN_CLICKED(IDC_BUTTON_SCAN_DVR, OnButtonScanDvr)
	ON_BN_CLICKED(IDC_BUTTON_SCAN_STOP, OnButtonScanStop)
	ON_MESSAGE(WM_SCAN_FOUND, OnScanFound)
	ON_MESSAGE(WM_SCAN_STATUS, OnScanStatus)
	ON_MESSAGE(WM_SCAN_DONE, OnScanDone)
END_MESSAGE_MAP()

BOOL CCameraScanDlg::OnInitDialog()
{
	CDialog::OnInitDialog();

	// List
	CListCtrl* pList = (CListCtrl*)GetDlgItem(IDC_LIST_FOUND_CAMERAS);
	pList->SetExtendedStyle(pList->GetExtendedStyle() | LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT);
	pList->InsertColumn(0, _T("Endere\u00e7o"), LVCFMT_LEFT, 175);
	pList->InsertColumn(1, _T("Tipo"), LVCFMT_LEFT, 75);
	pList->InsertColumn(2, _T("Informa\u00e7\u00f5es"), LVCFMT_LEFT, 200);

	// Login and DVR address
	GetDlgItem(IDC_AUTH_USERNAME)->SetWindowText(m_sUser);
	GetDlgItem(IDC_AUTH_PASSWORD)->SetWindowText(m_sPass);
	CString sDvr(m_sDvrHost);
	if (!sDvr.IsEmpty() && m_nDvrPort != RTSP_DEFAULT_PORT)
		sDvr.Format(_T("%s:%d"), m_sDvrHost, m_nDvrPort);
	if (GetDlgItem(IDC_EDIT_SCAN_DVR))
		GetDlgItem(IDC_EDIT_SCAN_DVR)->SetWindowText(sDvr);

	// Already known results
	for (int i = 0; i < m_Found.GetSize(); i++)
		AddItem(m_Found[i]);
	if (m_Found.GetSize() > 0)
	{
		CString sStatus;
		sStatus.Format(_T("%d c\u00e2mera(s) ou canal(is) encontrado(s), marque as que deseja abrir."), (int)m_Found.GetSize());
		GetDlgItem(IDC_STATIC_SCAN_STATUS)->SetWindowText(sStatus);
	}

	if (m_bSpy)
		InitSpy();
	EnableDisableCtrls();
	if (m_bAutoScanLan)
		StartScan(TRUE, !m_sDvrHost.IsEmpty());
	else if (m_bAutoScanDvr)
		StartScan(FALSE, TRUE);

	return TRUE;
}

void CCameraScanDlg::EnableDisableCtrls()
{
	GetDlgItem(IDC_BUTTON_SCAN_LAN)->EnableWindow(!m_bScanning);
	if (GetDlgItem(IDC_BUTTON_SCAN_DVR))
		GetDlgItem(IDC_BUTTON_SCAN_DVR)->EnableWindow(!m_bScanning);
	GetDlgItem(IDC_BUTTON_SCAN_STOP)->EnableWindow(m_bScanning);
}

void CCameraScanDlg::AddItem(const CFoundCamera& cam)
{
	// Avoid duplicates
	for (int i = 0; i < m_Items.GetSize(); i++)
	{
		if (m_Items[i].m_sHost == cam.m_sHost &&
			m_Items[i].m_nPort == cam.m_nPort &&
			m_Items[i].m_sUrl == cam.m_sUrl)
			return;
	}

	CListCtrl* pList = (CListCtrl*)GetDlgItem(IDC_LIST_FOUND_CAMERAS);
	CString sAddress;
	if (!cam.m_sUrl.IsEmpty())
		sAddress = cam.m_sUrl;
	else
		sAddress.Format(_T("%s:%d"), cam.m_sHost, cam.m_nPort);
	BOOL bRunning = CHostPortDlg::IsRunningQuiet(	cam.m_sUrl.IsEmpty() ? cam.m_sHost : cam.m_sUrl,
													cam.m_nPort,
													(cam.m_sUrl.IsEmpty() && cam.m_nMode >= 0) ? cam.m_nMode : (int)CVideoDeviceDoc::GENERIC_1_RTSP);
	CString sInfo(cam.m_sInfo);
	if (bRunning)
		sInfo = _T("j\u00e1 aberta, ") + sInfo;
	int nIndex = pList->InsertItem(pList->GetItemCount(), sAddress);
	pList->SetItemText(nIndex, 1, cam.m_sType);
	pList->SetItemText(nIndex, 2, sInfo);
	pList->SetItemData(nIndex, (DWORD_PTR)m_Items.Add(cam));
	pList->SetCheck(nIndex, (!bRunning && (!cam.m_sUrl.IsEmpty() || cam.m_nMode >= 0)) ? TRUE : FALSE);
}

void CCameraScanDlg::StartScan(BOOL bLan, BOOL bDvr)
{
	StopScan();

	GetDlgItem(IDC_AUTH_USERNAME)->GetWindowText(m_sUser);
	GetDlgItem(IDC_AUTH_PASSWORD)->GetWindowText(m_sPass);

	CScanJob* pJob = new CScanJob;
	pJob->m_bSpy = FALSE;
	pJob->m_nMakerMode = -1;
	pJob->m_dwFirstIp = 0;
	pJob->m_dwLastIp = 0;
	pJob->m_bLan = bLan;
	pJob->m_bDvr = bDvr;
	pJob->m_nPort = RTSP_DEFAULT_PORT;
	pJob->m_sUser = m_sUser;
	pJob->m_sPass = m_sPass;
	pJob->m_sExclude = m_sExcludeHost;
	// IP address with optional port: host or host:port
	CString sDvr;
	GetDlgItem(IDC_EDIT_SCAN_DVR)->GetWindowText(sDvr);
	sDvr.Trim();
	if (bDvr && sDvr.IsEmpty())
	{
		GetDlgItem(IDC_STATIC_SCAN_STATUS)->SetWindowText(_T("Digite primeiro o IP do DVR (host ou host:porta)."));
		delete pJob;
		return;
	}
	int nColon = sDvr.Find(_T(':'));
	if (nColon >= 0)
	{
		int nPort = _ttoi(sDvr.Mid(nColon + 1));
		if (nPort > 0 && nPort <= 65535)
			pJob->m_nPort = nPort;
		sDvr = sDvr.Left(nColon);
	}
	pJob->m_sHost = sDvr;
	pJob->m_hWnd = GetSafeHwnd();
	pJob->m_plCancel = &m_lCancel;
	m_lCancel = 0;

	m_pScanThread = ::AfxBeginThread(DlgScanThreadProc, pJob, THREAD_PRIORITY_BELOW_NORMAL, 0, CREATE_SUSPENDED);
	if (!m_pScanThread)
	{
		delete pJob;
		return;
	}
	m_pScanThread->m_bAutoDelete = FALSE;
	m_pScanThread->ResumeThread();
	m_bScanning = TRUE;
	EnableDisableCtrls();
}

void CCameraScanDlg::StopScan()
{
	if (m_pScanThread)
	{
		// All the network operations have a timeout, the thread ends quickly
		::InterlockedExchange(&m_lCancel, 1);
		::WaitForSingleObject(m_pScanThread->m_hThread, INFINITE);
		delete m_pScanThread;
		m_pScanThread = NULL;
	}
	m_bScanning = FALSE;
}

void CCameraScanDlg::OnButtonScanLan()
{
	if (m_bSpy)
	{
		StartSpyScan();
		return;
	}
	// Empty IP field: sweep the whole network. IP given: sweep the network and scan that DVR
	CString sIp;
	GetDlgItem(IDC_EDIT_SCAN_DVR)->GetWindowText(sIp);
	sIp.Trim();
	StartScan(TRUE, !sIp.IsEmpty());
}

void CCameraScanDlg::OnButtonScanDvr()
{
	StartScan(FALSE, TRUE);
}

void CCameraScanDlg::OnButtonScanStop()
{
	StopScan();
	GetDlgItem(IDC_STATIC_SCAN_STATUS)->SetWindowText(_T("Varredura interrompida."));
	EnableDisableCtrls();
}

LRESULT CCameraScanDlg::OnScanFound(WPARAM wparam, LPARAM lparam)
{
	CFoundCamera* p = (CFoundCamera*)lparam;
	if (p)
	{
		AddItem(*p);
		delete p;
	}
	return 0;
}

LRESULT CCameraScanDlg::OnScanStatus(WPARAM wparam, LPARAM lparam)
{
	CString* p = (CString*)lparam;
	if (p)
	{
		GetDlgItem(IDC_STATIC_SCAN_STATUS)->SetWindowText(*p);
		delete p;
	}
	return 0;
}

LRESULT CCameraScanDlg::OnScanDone(WPARAM wparam, LPARAM lparam)
{
	StopScan(); // the thread has already finished
	EnableDisableCtrls();
	return 0;
}

void CCameraScanDlg::OnOK()
{
	StopScan();
	if (m_bSpy)
	{
		SaveSpySettings();
		m_nSpyTransport = (((CComboBox*)GetDlgItem(IDC_SPY_TRANSPORT))->GetCurSel() == 2) ? 0 : 1; // automatic and TCP: TCP first
	}
	GetDlgItem(IDC_AUTH_USERNAME)->GetWindowText(m_sUser);
	GetDlgItem(IDC_AUTH_PASSWORD)->GetWindowText(m_sPass);

	CWaitCursor WaitCursor;
	CListCtrl* pList = (CListCtrl*)GetDlgItem(IDC_LIST_FOUND_CAMERAS);
	m_nOpened = 0;
	for (int i = 0; i < pList->GetItemCount(); i++)
	{
		if (pList->GetCheck(i))
		{
			int nItem = (int)pList->GetItemData(i);
			if (nItem >= 0 && nItem < m_Items.GetSize())
			{
				CFoundCamera cam(m_Items[nItem]);

				// Yoosee/Gwell cameras only work with the path /onvif1 (Generic /onvif1, RTSP Transport unchecked)
				if (cam.m_sInfo.Find(_T("Yoosee")) != -1 || cam.m_sInfo.Find(_T("Gwell")) != -1)
					cam.m_sUrl.Format(_T("rtsp://%s:%d/onvif1"), cam.m_sHost, cam.m_nPort);
				// Fill the camera maker settings automatically from the stream path
				// (done by OpenNetworkCamera with CCameraScanner::ModeFromRtspPath())

				int nForceMode = (cam.m_sUrl.IsEmpty() && cam.m_nMode >= 0) ? cam.m_nMode : -1;
				if (CHostPortDlg::OpenNetworkCamera(cam.m_sHost, cam.m_nPort, cam.m_sUrl, m_sUser, m_sPass, nForceMode, m_nSpyTransport))
					m_nOpened++;
			}
		}
	}
	if (m_nOpened == 0)
	{
		GetDlgItem(IDC_STATIC_SCAN_STATUS)->SetWindowText(_T("Marque pelo menos uma c\u00e2mera que ainda n\u00e3o esteja aberta."));
		EnableDisableCtrls();
		return;
	}
	CDialog::OnOK();
}

void CCameraScanDlg::OnCancel()
{
	StopScan();
	CDialog::OnCancel();
}

/////////////////////////////////////////////////////////////////////////////
// Spy dialog

static BOOL ParsePortList(const CString& sText, CArray<int, int>& aPorts)
{
	aPorts.RemoveAll();
	int nPos = 0;
	CString sToken = sText.Tokenize(_T(", ;"), nPos);
	while (!sToken.IsEmpty())
	{
		int nPort = _ttoi(sToken);
		if (nPort > 0 && nPort <= 65535 && aPorts.GetSize() < 32)
			aPorts.Add(nPort);
		sToken = sText.Tokenize(_T(", ;"), nPos);
	}
	return aPorts.GetSize() > 0;
}

void CCameraScanDlg::InitSpy()
{
	CWinApp* pApp = ::AfxGetApp();

	// First and last IP (full addresses), the default is the range .1 to .100 of the first local network
	CString sFrom = pApp->GetProfileString(_T("SpyDlg"), _T("FromIp"), _T(""));
	CString sTo = pApp->GetProfileString(_T("SpyDlg"), _T("ToIp"), _T(""));
	if (sFrom.IsEmpty() || sTo.IsEmpty())
	{
		CArray<DWORD, DWORD> aIps;
		CArray<DWORD, DWORD> aMasks;
		GetLocalIpv4(aIps, aMasks);
		if (aIps.GetSize() > 0)
		{
			sFrom.Format(_T("%u.%u.%u.1"), (aIps[0] >> 24) & 0xFF, (aIps[0] >> 16) & 0xFF, (aIps[0] >> 8) & 0xFF);
			sTo.Format(_T("%u.%u.%u.100"), (aIps[0] >> 24) & 0xFF, (aIps[0] >> 16) & 0xFF, (aIps[0] >> 8) & 0xFF);
		}
	}
	GetDlgItem(IDC_SPY_FROM)->SetWindowText(sFrom);
	GetDlgItem(IDC_SPY_TO)->SetWindowText(sTo);
	GetDlgItem(IDC_SPY_PORTS)->SetWindowText(pApp->GetProfileString(_T("SpyDlg"), _T("Ports"), _T("554, 10554, 8554, 5543, 5544, 555, 7070, 88, 8000")));

	// Camera makers: the first entry is the automatic mode
	CComboBox* pMaker = (CComboBox*)GetDlgItem(IDC_SPY_MAKER);
	int nIndex = pMaker->AddString(_T("Autom\u00e1tico (procura o caminho)"));
	pMaker->SetItemData(nIndex, (DWORD_PTR)-1);
	CArray<CDeviceTypeModePair> a;
	CHostPortDlg::InitDeviceTypeModeArrayForCB(a);
	int nSel = 0;
	int nSavedMode = pApp->GetProfileInt(_T("SpyDlg"), _T("Maker"), -1);
	for (int i = 0; i < a.GetSize(); i++)
	{
		if (a[i].m_dwMode >= (DWORD)CVideoDeviceDoc::URL_RTSP)
		{
			nIndex = pMaker->AddString(a[i].m_sMode);
			pMaker->SetItemData(nIndex, (DWORD_PTR)a[i].m_dwMode);
			if ((int)a[i].m_dwMode == nSavedMode)
				nSel = nIndex;
		}
	}
	pMaker->SetCurSel(nSel);

	// Transport
	CComboBox* pTransport = (CComboBox*)GetDlgItem(IDC_SPY_TRANSPORT);
	pTransport->AddString(_T("Autom\u00e1tico (TCP, sen\u00e3o UDP)"));
	pTransport->AddString(_T("TCP"));
	pTransport->AddString(_T("UDP"));
	pTransport->SetCurSel(pApp->GetProfileInt(_T("SpyDlg"), _T("Transport"), 0));
}

void CCameraScanDlg::SaveSpySettings()
{
	CWinApp* pApp = ::AfxGetApp();
	CString s;
	GetDlgItem(IDC_SPY_FROM)->GetWindowText(s);
	pApp->WriteProfileString(_T("SpyDlg"), _T("FromIp"), s);
	GetDlgItem(IDC_SPY_TO)->GetWindowText(s);
	pApp->WriteProfileString(_T("SpyDlg"), _T("ToIp"), s);
	GetDlgItem(IDC_SPY_PORTS)->GetWindowText(s);
	pApp->WriteProfileString(_T("SpyDlg"), _T("Ports"), s);
	CComboBox* pMaker = (CComboBox*)GetDlgItem(IDC_SPY_MAKER);
	if (pMaker->GetCurSel() >= 0)
		pApp->WriteProfileInt(_T("SpyDlg"), _T("Maker"), (int)pMaker->GetItemData(pMaker->GetCurSel()));
	pApp->WriteProfileInt(_T("SpyDlg"), _T("Transport"), ((CComboBox*)GetDlgItem(IDC_SPY_TRANSPORT))->GetCurSel());
}

void CCameraScanDlg::StartSpyScan()
{
	StopScan();
	GetDlgItem(IDC_AUTH_USERNAME)->GetWindowText(m_sUser);
	GetDlgItem(IDC_AUTH_PASSWORD)->GetWindowText(m_sPass);

	// First and last IP, for example 192.168.1.0 to 192.168.1.10
	CString sFrom, sTo;
	GetDlgItem(IDC_SPY_FROM)->GetWindowText(sFrom);
	GetDlgItem(IDC_SPY_TO)->GetWindowText(sTo);
	sFrom.Trim();
	sTo.Trim();
	unsigned long ulFrom = ::inet_addr(CStringA(sFrom));
	unsigned long ulTo = ::inet_addr(CStringA(sTo));
	if (ulFrom == INADDR_NONE || ulTo == INADDR_NONE || ulFrom == 0 || ulTo == 0)
	{
		GetDlgItem(IDC_STATIC_SCAN_STATUS)->SetWindowText(_T("IP inv\u00e1lido: digite o IP completo, por exemplo 192.168.1.0 e 192.168.1.10"));
		return;
	}
	DWORD dwFrom = ntohl(ulFrom);
	DWORD dwTo = ntohl(ulTo);
	if (dwFrom > dwTo)
	{
		DWORD dwTmp = dwFrom;
		dwFrom = dwTo;
		dwTo = dwTmp;
	}

	CScanJob* pJob = new CScanJob;
	pJob->m_bSpy = TRUE;
	pJob->m_bLan = FALSE;
	pJob->m_bDvr = FALSE;
	pJob->m_nPort = RTSP_DEFAULT_PORT;
	pJob->m_dwFirstIp = dwFrom;
	pJob->m_dwLastIp = dwTo;
	CString sPorts;
	GetDlgItem(IDC_SPY_PORTS)->GetWindowText(sPorts);
	if (!ParsePortList(sPorts, pJob->m_aPorts))
	{
		GetDlgItem(IDC_STATIC_SCAN_STATUS)->SetWindowText(_T("Digite pelo menos uma porta, por exemplo 554, 10554"));
		delete pJob;
		return;
	}
	CComboBox* pMaker = (CComboBox*)GetDlgItem(IDC_SPY_MAKER);
	pJob->m_nMakerMode = -1;
	if (pMaker->GetCurSel() > 0)
	{
		pJob->m_nMakerMode = (int)pMaker->GetItemData(pMaker->GetCurSel());
		pMaker->GetLBText(pMaker->GetCurSel(), pJob->m_sMakerName);
	}
	pJob->m_sUser = m_sUser;
	pJob->m_sPass = m_sPass;
	pJob->m_hWnd = GetSafeHwnd();
	pJob->m_plCancel = &m_lCancel;
	m_lCancel = 0;
	SaveSpySettings();

	m_pScanThread = ::AfxBeginThread(DlgScanThreadProc, pJob, THREAD_PRIORITY_BELOW_NORMAL, 0, CREATE_SUSPENDED);
	if (!m_pScanThread)
	{
		delete pJob;
		return;
	}
	m_pScanThread->m_bAutoDelete = FALSE;
	m_pScanThread->ResumeThread();
	m_bScanning = TRUE;
	EnableDisableCtrls();
}


#endif
