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

// Ports scanned on each host of the local network (RTSP only, the ONVIF
// discovery finds the cameras that are not listening on one of these)
// The first LAN_RTSP_PORTS_COUNT ports are RTSP ports, the last two are the HTTP and the
// stream port of the ESP32-CAM (esp32-cam-webserver). The open ports are stored in a BYTE mask
static const int LAN_PORTS[] = { 554, 8554, 10554, 5543, 555, 7070, 80, 81 };
static const int LAN_PORTS_COUNT = sizeof(LAN_PORTS) / sizeof(LAN_PORTS[0]);
#define LAN_RTSP_PORTS_COUNT	6
#define LAN_PORT_IDX_HTTP		6
#define LAN_PORT_IDX_STREAM		7

// Stream paths that are tried to find out the RTSP url of a camera
static const LPCTSTR RTSP_PATHS[] =
{
	_T("/11"),
	_T("/onvif1"),
	_T("/live/ch0"),
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
	_T("/ch01.264")
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

// Returns 1 if a stream path has been found, 0 if not found, -1 if a login is required or wrong
static int FindRtspPath(const SOCKADDR_STORAGE& addr,
						int nAddrLen,
						const CString& sHost,
						int nPort,
						const CString& sUser,
						const CString& sPass,
						CScanSink& sink,
						CString& sPathOut)
{
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
			sInfo = _T("ONVIF device");
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
	cam.m_sInfo = (sName.IsEmpty() ? CString() : sName + _T(", ")) + _T("HTTP motion jpeg, one viewer at a time, snapshot: /capture");
	return TRUE;
}

/////////////////////////////////////////////////////////////////////////////
// CCameraScanner

void CCameraScanner::ScanLan(	CScanSink& sink,
								const CStringArray& saExcludeHosts,
								const CString& sUser,
								const CString& sPass)
{
	// Local subnets
	CArray<DWORD, DWORD> aLocalIps;
	CArray<DWORD, DWORD> aLocalMasks;
	GetLocalIpv4(aLocalIps, aLocalMasks);
	if (aLocalIps.GetSize() == 0)
	{
		sink.OnStatus(_T("No active network connection found."));
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

	// ONVIF discovery
	CMapStringToString mapOnvif;
	sink.OnStatus(_T("Searching ONVIF cameras..."));
	WsDiscovery(sink, aLocalIps, mapOnvif);
	if (sink.IsCancelled())
		return;

	// Scan the RTSP ports of all hosts in batches of 60 non blocking connections (FD_SETSIZE is 64)
	const int nHostCount = (int)aHosts.GetSize();
	const int nTotal = nHostCount * LAN_PORTS_COUNT;
	const int BATCH = 60;
	CArray<BYTE, BYTE> aOpenMask; // bit mask of the open ports of each host
	aOpenMask.SetSize(nHostCount);
	for (i = 0; i < nHostCount; i++)
		aOpenMask[i] = 0;
	for (int nStart = 0; nStart < nTotal; nStart += BATCH)
	{
		if (sink.IsCancelled())
			return;
		CString sStatus;
		sStatus.Format(_T("Scanning the network for RTSP ports... %d%%"), nStart * 100 / nTotal);
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
				aOpenMask[nHost] = (BYTE)(aOpenMask[nHost] | (1 << nPortIdx));
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
						aOpenMask[nHost] = (BYTE)(aOpenMask[nHost] | (1 << nPortIdx));
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

		int nRtspPort = 0;
		for (int p = 0; p < LAN_RTSP_PORTS_COUNT && nRtspPort == 0; p++)
		{
			if (aOpenMask[nHost] & (1 << p))
			{
				if (ProbeTcpPort(addr, nAddrLen, LAN_PORTS[p], sHostA) == 1)
					nRtspPort = LAN_PORTS[p];
			}
		}
		if (nRtspPort == 0)
			continue;

		CFoundCamera cam;
		cam.m_sHost = sHost;
		cam.m_nPort = nRtspPort;
		cam.m_sType = _T("RTSP");
		CString sOnvifInfo;
		if (mapOnvif.Lookup(sHost, sOnvifInfo))
		{
			cam.m_sType = _T("RTSP + ONVIF");
			cam.m_sInfo = sOnvifInfo;
			mapOnvif.RemoveKey(sHost);
		}

		CString sStatus;
		sStatus.Format(_T("Camera found at %s, looking for the stream path..."), sHost);
		sink.OnStatus(sStatus);
		CString sPath;
		int nRes = FindRtspPath(addr, nAddrLen, sHost, nRtspPort, sUser, sPass, sink, sPath);
		CString sNote;
		if (nRes == 1)
		{
			int nDummyPort;
			int nDummyMode;
			CHostPortDlg::MakeUrl(sHost, nRtspPort, sPath, CVideoDeviceDoc::URL_RTSP, cam.m_sUrl, nDummyPort, nDummyMode);
			sNote = _T("path ") + sPath;
		}
		else if (nRes < 0)
		{
			sNote = sUser.IsEmpty() ? _T("login required") : _T("login rejected");
			if (cam.m_sInfo.Find(_T("Yoosee")) >= 0)
				sNote += _T(" (Yoosee: user admin and the RTSP password set in the Yoosee app)");
		}
		else
			sNote = _T("stream path unknown, choose the camera maker");
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
		cam.m_sInfo = sInfo + _T(", RTSP port not detected");
		if (sInfo.Find(_T("Yoosee")) >= 0)
			cam.m_sInfo += _T(" (Yoosee: enable RTSP in the Yoosee app)");
		sink.OnFound(cam);
		nFound++;
	}

	CString sDone;
	sDone.Format(_T("Network scan finished: %d camera(s) found."), nFound);
	sink.OnStatus(sDone);
}

void CCameraScanner::ScanDvr(	CScanSink& sink,
								const CString& sHost,
								int nPort,
								const CString& sUser,
								const CString& sPass)
{
	SOCKADDR_STORAGE addr;
	int nAddrLen;
	if (!ResolveHost(sHost, addr, nAddrLen))
	{
		sink.OnStatus(_T("Cannot resolve ") + sHost);
		return;
	}

	// Find the channel path format of this DVR/NVR
	sink.OnStatus(_T("Detecting the DVR stream format..."));
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
			sink.OnStatus(sUser.IsEmpty() ?	_T("The DVR requires a user name and a password.") :
											_T("DVR login rejected: check user name and password."));
			return;
		}
		else if (nCode == 0 && t == 0)
		{
			CString sMsg;
			sMsg.Format(_T("No RTSP answer from %s port %d."), sHost, nPort);
			sink.OnStatus(sMsg);
			return;
		}
	}
	if (nTemplate < 0)
	{
		sink.OnStatus(_T("No known DVR channel format found: enter the DVR as a camera and choose its maker."));
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
		sStatus.Format(_T("Checking DVR channel %d..."), nCh);
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
			cam.m_sType.Format(_T("DVR ch %d"), nCh);
			cam.m_sInfo = DVR_TEMPLATES[nTemplate].m_sName;
			sink.OnFound(cam);
			nFound++;
			nMisses = 0;
		}
		else
			nMisses++;
	}
	CString sDone;
	sDone.Format(_T("DVR scan finished: %d channel(s) found."), nFound);
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
	CCameraScanner::ScanLan(*pCtx, saExclude, pCtx->m_sUser, pCtx->m_sPass);

	// A DVR/NVR shows its channels on the same address,
	// report them only if there are at least two
	if (pCtx->m_nPort > 0 && !pCtx->IsCancelled())
	{
		CCollectSink DvrSink(*pCtx);
		CCameraScanner::ScanDvr(DvrSink, pCtx->m_sHost, pCtx->m_nPort, pCtx->m_sUser, pCtx->m_sPass);
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
	if (pJob->m_bDvr)
		CCameraScanner::ScanDvr(sink, pJob->m_sHost, pJob->m_nPort, pJob->m_sUser, pJob->m_sPass);
	else
	{
		CStringArray saExclude;
		if (!pJob->m_sExclude.IsEmpty())
			saExclude.Add(pJob->m_sExclude);
		CCameraScanner::ScanLan(sink, saExclude, pJob->m_sUser, pJob->m_sPass);
	}
	::PostMessage(pJob->m_hWnd, WM_SCAN_DONE, 0, 0);
	delete pJob;
	return 0;
}

/////////////////////////////////////////////////////////////////////////////
// CCameraScanDlg dialog

CCameraScanDlg::CCameraScanDlg(CWnd* pParent /*=NULL*/)
	: CDialog(CCameraScanDlg::IDD, pParent)
{
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
	pList->InsertColumn(0, _T("Address"), LVCFMT_LEFT, 175);
	pList->InsertColumn(1, _T("Type"), LVCFMT_LEFT, 75);
	pList->InsertColumn(2, _T("Info"), LVCFMT_LEFT, 200);

	// Login and DVR address
	GetDlgItem(IDC_AUTH_USERNAME)->SetWindowText(m_sUser);
	GetDlgItem(IDC_AUTH_PASSWORD)->SetWindowText(m_sPass);
	CString sDvr(m_sDvrHost);
	if (!sDvr.IsEmpty() && m_nDvrPort != RTSP_DEFAULT_PORT)
		sDvr.Format(_T("%s:%d"), m_sDvrHost, m_nDvrPort);
	GetDlgItem(IDC_EDIT_SCAN_DVR)->SetWindowText(sDvr);

	// Already known results
	for (int i = 0; i < m_Found.GetSize(); i++)
		AddItem(m_Found[i]);
	if (m_Found.GetSize() > 0)
	{
		CString sStatus;
		sStatus.Format(_T("%d camera(s) or channel(s) found, check the ones to open."), (int)m_Found.GetSize());
		GetDlgItem(IDC_STATIC_SCAN_STATUS)->SetWindowText(sStatus);
	}

	EnableDisableCtrls();
	if (m_bAutoScanLan)
		StartScan(FALSE);
	else if (m_bAutoScanDvr)
		StartScan(TRUE);

	return TRUE;
}

void CCameraScanDlg::EnableDisableCtrls()
{
	GetDlgItem(IDC_BUTTON_SCAN_LAN)->EnableWindow(!m_bScanning);
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
													CVideoDeviceDoc::GENERIC_1_RTSP);
	CString sInfo(cam.m_sInfo);
	if (bRunning)
		sInfo = _T("already open, ") + sInfo;
	int nIndex = pList->InsertItem(pList->GetItemCount(), sAddress);
	pList->SetItemText(nIndex, 1, cam.m_sType);
	pList->SetItemText(nIndex, 2, sInfo);
	pList->SetItemData(nIndex, (DWORD_PTR)m_Items.Add(cam));
	pList->SetCheck(nIndex, (!bRunning && !cam.m_sUrl.IsEmpty()) ? TRUE : FALSE);
}

void CCameraScanDlg::StartScan(BOOL bDvr)
{
	StopScan();

	GetDlgItem(IDC_AUTH_USERNAME)->GetWindowText(m_sUser);
	GetDlgItem(IDC_AUTH_PASSWORD)->GetWindowText(m_sPass);

	CScanJob* pJob = new CScanJob;
	pJob->m_bDvr = bDvr;
	pJob->m_nPort = RTSP_DEFAULT_PORT;
	pJob->m_sUser = m_sUser;
	pJob->m_sPass = m_sPass;
	pJob->m_sExclude = m_sExcludeHost;
	if (bDvr)
	{
		// DVR address with optional port: host or host:port
		CString sDvr;
		GetDlgItem(IDC_EDIT_SCAN_DVR)->GetWindowText(sDvr);
		sDvr.Trim();
		if (sDvr.IsEmpty())
		{
			GetDlgItem(IDC_STATIC_SCAN_STATUS)->SetWindowText(_T("Enter the DVR IP address first (host or host:port)."));
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
	}
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
	StartScan(FALSE);
}

void CCameraScanDlg::OnButtonScanDvr()
{
	StartScan(TRUE);
}

void CCameraScanDlg::OnButtonScanStop()
{
	StopScan();
	GetDlgItem(IDC_STATIC_SCAN_STATUS)->SetWindowText(_T("Scan stopped."));
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
				if (CHostPortDlg::OpenNetworkCamera(cam.m_sHost, cam.m_nPort, cam.m_sUrl, m_sUser, m_sPass))
					m_nOpened++;
			}
		}
	}
	if (m_nOpened == 0)
	{
		GetDlgItem(IDC_STATIC_SCAN_STATUS)->SetWindowText(_T("Check at least one camera that is not already open."));
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

#endif
