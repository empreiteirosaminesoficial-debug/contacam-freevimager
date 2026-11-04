from pathlib import Path
p = Path('/home/ubuntu/contacam-freevimager/VideoDeviceDoc.cpp')
s = p.read_text()
needle = 'pAutoFixCtx->m_sPass = m_pDoc->m_sHttpGetFramePassword;'
i = s.index(needle, s.index('CWatchdogThread::Work')) + len(needle)
s = s[:i] + '\n\t\t\t\t\t\tpAutoFixCtx->m_sFallbackUrl = ::AfxGetApp()->GetProfileString(pAutoFixCtx->m_sDevicePathName, _T("AutoSpyFallbackUrl"), _T(""));\n\t\t\t\t\t\tpAutoFixCtx->m_bAutomatic = TRUE;\n\t\t\t\t\t\tpAutoFixCtx->m_bSkipOld = TRUE;' + s[i:]
anchor = '\n\t\t\t}\n\t\t\t::LogLine(_T("%s %s (%dx%d, %s)"), ML_STRING(1764, "Starting")'
pos = s.index(anchor, s.index('// First successful connection of a new network camera'))
insert = r'''
                // Learn one verified alternate RTSP path/port for future silent recovery.
                if (!m_pDxCapture && !m_sGetFrameVideoHost.IsEmpty() && m_nNetworkDeviceTypeMode >= URL_RTSP && !((CUImagerApp*)::AfxGetApp())->m_bServiceProcess)
                {
                    CAutoFixCtx* pLearnCtx = new CAutoFixCtx;
                    pLearnCtx->m_sDevicePathName = GetDevicePathName();
                    pLearnCtx->m_sHost = m_sGetFrameVideoHost;
                    pLearnCtx->m_nOldPort = (int)m_nGetFrameVideoPort;
                    pLearnCtx->m_nOldMode = (int)m_nNetworkDeviceTypeMode;
                    pLearnCtx->m_sUser = m_sHttpGetFrameUsername;
                    pLearnCtx->m_sPass = m_sHttpGetFramePassword;
                    pLearnCtx->m_bSkipOld = TRUE;
                    pLearnCtx->m_bLearnOnly = TRUE;
                    if (!::PostMessage(::AfxGetMainFrame()->GetSafeHwnd(), WM_THREADSAFE_AUTOFIX_START, 2, (LPARAM)pLearnCtx))
                        delete pLearnCtx;
                }'''.replace('                ', '\t\t\t\t')
s = s[:pos] + insert + s[pos:]
p.write_text(s)
