
#include "Proxy.hpp"
#include "DNSCache.hpp"

#include <Ws2tcpip.h> // for getaddrinfo()
#include <cstdio> // for sprintf_s()

#include <sstream>
#include <cassert>


//////////////////////////////////////////////////////////////////////////

string ToLower(const string &s) {
    string ret(s);

    for (auto &ch : ret) {
        ch = tolower(ch);
    }

    return ret;
}

//////////////////////////////////////////////////////////////////////////

MyProxy::Statistics MyProxy::ms_stat;

MyProxy::MyProxy(SOCKET bsocket)
    : m_bsocket(bsocket), m_ssocket(INVALID_SOCKET) {

}

MyProxy::~MyProxy() {
    ShutdownServerSocket();
}

bool MyProxy::HandleBrowser() {
    const u_long nBufferSize = GetBestBufferSize(m_bsocket);
    if (nBufferSize == -1) {
        return false;
    }

    Buffer buf(nBufferSize);
    int nReadBytes;

    do {
        nReadBytes = recv(m_bsocket, buf.data(), nBufferSize, 0);
        if (nReadBytes > 0) {
            m_vbuf.insert(m_vbuf.end(), buf.data(), buf.data() + nReadBytes);
            m_vbuf.push_back(0);

            if (!m_headers.Parse(m_vbuf.data(), true)) {
                m_vbuf.pop_back(); // 移除末尾的 '\0'
                continue;
            }

            PrintRequest(Logger::OL_INFO);

            //----------------------------------------

            Host lastHost = m_host;

            if (strncmp(m_vbuf.data(), "CONNECT ", 8) == 0) {
                SplitHost(m_vbuf.data() + 8, 443);

                return RelaySSLConnection();
            }
            else {
                SplitHost(m_headers.m["Host"], 80);
            }

            if (lastHost != m_host && m_ssocket != INVALID_SOCKET) {
                ShutdownServerSocket();
            }

            switch (HandleServer()) {
            case RR_ALIVE:
                m_vbuf.clear();
                m_headers.Clear();
                // 不要重置 m_host 与 m_ssocket

                continue;

            case RR_CLOSE:
                return true;

            case RR_ERROR:
            default:
                return false;
            }
        }
        else if (nReadBytes == SOCKET_ERROR) {
            LogError(WSAGetLastErrorMessage(__FUNC__ "recv() failed"));
            return false;
        }
    } while (nReadBytes != 0);

    LogInfo(__FUNC__ "Connection closed by browser");
    return true;
}

void MyProxy::PrintRequest(Logger::OutputLevel level) const {
    Log(string(m_vbuf.data(), strchr(m_vbuf.data(), '\r')), level);
}

MyProxy::Statistics MyProxy::GetStatistics() {
    return ms_stat;
}

void MyProxy::SplitHost(const string &host_decl, int default_port) {
    m_host.port = default_port;

    auto pos = host_decl.find(':');
    if (pos != string::npos) {
        m_host.port = atoi(host_decl.c_str() + pos + 1);
    }
    else {
        pos = host_decl.length();
    }

    m_host.name = host_decl.substr(0, pos);
}

MyProxy::RelayResult MyProxy::HandleServer() {
    if (m_ssocket != INVALID_SOCKET) {
        RelayResult rr = DoHandleServer();
        if (rr == RR_ERROR) {
            ShutdownServerSocket(); // 忽略错误处理

            return DoHandleServer();
        }

        LogInfo(__FUNC__ "Successfully reused socket.");
        return rr;
    }

    return DoHandleServer();
}

MyProxy::RelayResult MyProxy::DoHandleServer() {
    if (m_ssocket == INVALID_SOCKET && !SetUpServerSocket()) {
        return RR_ERROR;
    }

    if (!SendBrowserHeaders()) {
        return RR_ERROR;
    }

    // 剩余的数据体
    auto rr = Write(m_ssocket, m_vbuf.data() + m_headers.bodyOffset,
                    m_vbuf.size() - 1 - m_headers.bodyOffset);

    if (rr != RR_ALIVE) {
        return rr;
    }

    if (!RelayToServer()) {
        return RR_ERROR;
    }

    if (!RelayToBrowser()) {
        return RR_ERROR;
    }

    return (m_ssocket != INVALID_SOCKET) ? RR_ALIVE : RR_CLOSE;
}

bool MyProxy::ShutdownServerSocket() {
    if (m_ssocket == INVALID_SOCKET) {
        return true;
    }

    auto ssocket = m_ssocket;
    m_ssocket = INVALID_SOCKET;

    return ShutdownConnection(ssocket, false);
}

SOCKET DoConnect(const addrinfo &ai) {
    SOCKET sd = socket(ai.ai_family, ai.ai_socktype, ai.ai_protocol);
    if (sd != INVALID_SOCKET) {
        if (connect(sd, ai.ai_addr, ai.ai_addrlen) == 0) {
            return sd;
        }

        closesocket(sd);
        sd = INVALID_SOCKET;
    }

    return sd;
}

bool MyProxy::SetUpServerSocket() {
    if (TryDNSCache()) {
        return true;
    }

    addrinfo hints, *result;

    memset(&hints, 0, sizeof(addrinfo));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    char portstr[6];
    sprintf_s(portstr, sizeof(portstr), "%d", m_host.port);

    // 将域名解释为一个 IP 地址链表
    if (getaddrinfo(m_host.name.c_str(), portstr, &hints, &result) != 0) {
        LogError(WSAGetLastErrorMessage(__FUNC__ "getaddrinfo() failed"));
        return false;
    }
    
    addrinfo *ai = result;

    do {
        m_ssocket = DoConnect(*ai);
        if (m_ssocket != INVALID_SOCKET) {
            DNSCache::Add(m_host.GetFullName(), *ai);

            freeaddrinfo(result);
            return true;
        }
    } while (ai = ai->ai_next);

    LogError(__FUNC__ "No appropriate IP address");

    freeaddrinfo(result);
    return false;
}

bool MyProxy::TryDNSCache() {
    ms_stat.dnsQueries++;

    auto fullName(m_host.GetFullName());
    auto entry = DNSCache::Resolve(fullName);
    if (entry) {
        m_ssocket = DoConnect(entry->ai);
        if (m_ssocket != INVALID_SOCKET) {
            ms_stat.dnsCacheHit++;
            return true;
        }

        // 删除失效条目
        DNSCache::Remove(fullName);
    }

    return false;
}

bool MyProxy::SendBrowserHeaders() {
    assert(!m_vbuf.empty());
    ostringstream ss;

    string first_line(m_vbuf.data(), strstr(m_vbuf.data(), "\r\n"));
    string needle(" http://" + m_headers.m["Host"]);

    auto pos = first_line.find(needle);
    if (pos != string::npos) {
        first_line.replace(pos, needle.length(), " ", 1);
    }

    ss << first_line << "\r\n";

    auto it(m_headers.m.find("Proxy-Connection"));
    if (it != m_headers.m.end()) {
        auto conn = it->second;
        m_headers.m.erase(it);

        if (m_headers.m.find("Connection") == m_headers.m.end()) {
            m_headers.m.emplace("Connection", conn);
        }
    }

    for (auto header : m_headers.m) {
        ss << header.first << ": " << header.second << "\r\n";
    }

    ss << "\r\n";

    auto s(ss.str());
    return Write(m_ssocket, s.c_str(), s.length()) == RR_ALIVE;
}

bool MyProxy::RelaySSLConnection() {
    if (!ShutdownServerSocket() || !SetUpServerSocket()) {
        return false;
    }

    const char *confirm = "HTTP/1.1 200 Connection Established\r\n\r\n";
    if (Write(m_bsocket, confirm, strlen(confirm)) != RR_ALIVE) {
        return false;
    }

    fd_set inputs;
    Buffer buf;

    while (true) {
        FD_ZERO(&inputs);
        FD_SET(m_bsocket, &inputs);
        FD_SET(m_ssocket, &inputs);

        int n = select(0, &inputs, nullptr, nullptr, nullptr);
        if (n > 0) {
            if (FD_ISSET(m_bsocket, &inputs)) {
                auto rr = SimpleRelay(m_bsocket, m_ssocket, buf);

                if (rr == RR_ERROR) {
                    auto fmt = __FUNC__ "SimpleRelay() browser failed";
                    LogError(WSAGetLastErrorMessage(fmt));

                    return false;
                }
                else if (rr == RR_CLOSE) {
                    return true;
                }
            }

            if (FD_ISSET(m_ssocket, &inputs)) {
                auto rr = SimpleRelay(m_ssocket, m_bsocket, buf);

                if (rr == RR_ERROR) {
                    auto fmt = __FUNC__ "SimpleRelay() server failed";
                    LogError(WSAGetLastErrorMessage(fmt));

                    return false;
                }
                else if (rr == RR_CLOSE) {
                    return true;
                }
            }
        }
        else { // SOCKET_ERROR
            LogError(WSAGetLastErrorMessage(__FUNC__ "select() failed"));
            return false;
        }
    } // while (true)
}

MyProxy::RelayResult MyProxy::SimpleRelay(SOCKET r, SOCKET w, Buffer &buf) {
    const u_long nBufferSize = GetBestBufferSize(r);
    if (nBufferSize == -1) {
        return RR_ERROR;
    }

    buf.resize(nBufferSize);

    int nRx = recv(r, buf.data(), nBufferSize, 0);
    if (nRx > 0) {
        return Write(w, buf.data(), nRx);
    }
    else if (nRx == 0) {
        return RR_CLOSE; // 连接被一方关闭
    }
    else { // SOCKET_ERROR
        return RR_ERROR;
    }
}

bool MyProxy::RelayToServer() {
    auto it(m_headers.m.find("Content-Length"));
    if (it == m_headers.m.end()) {
        return true;
    }

    int nContentLength = atoi(it->second.c_str());
    int nTotalRx = m_headers.bodyOffset + nContentLength;
    int nRest = nTotalRx - (m_vbuf.size() - 1);

    if (nRest == 0) {
        return true;
    }

    if (nRest < 0) {
        LogError(__FUNC__ "Invalid `Content-Length`.");
        return true;
    }

    const u_long nBufferSize = GetBestBufferSize(m_bsocket);
    if (nBufferSize == -1) {
        return false;
    }

    Buffer buf(nBufferSize);

    while (nRest > 0) {
        int n = recv(m_bsocket, buf.data(), min(nRest, (int) nBufferSize), 0);
        if (n > 0) {
            if (Write(m_ssocket, buf.data(), n) != RR_ALIVE) {
                return false;
            }

            nRest -= n;
        }
        else if (n == SOCKET_ERROR) {
            LogError(WSAGetLastErrorMessage(__FUNC__ "recv() failed"));
            return false;
        }
        else {
            // Browser closed connection before we could relay
            // all the data it sent, so bomb out early.
            LogError("Browser unexpectedly dropped connection!");
            return true;
        }
    }

    return true;
}

bool MyProxy::RelayToBrowser() {
    bool bHeadersParsed = false;

    Buffer hbuf; // Header buffer
    Headers headers;
    bool chunked = false;

    Buffer buf(0); // 先不要分配任何空间
    int nReadBytes;
    int nRest = -1;

    do {
        const u_long nBufferSize = GetBestBufferSize(m_ssocket);
        if (nBufferSize == -1) {
            return false;
        }

        buf.resize(nBufferSize + 1);
        nReadBytes = recv(m_ssocket, buf.data(), nBufferSize, 0);

        if (nReadBytes > 0) {
            buf[nReadBytes] = 0;
            buf.resize(nReadBytes + 1);

            if (!bHeadersParsed) {
                // 注意插入后 hbuf 也会直接以 '\0' 结尾
                hbuf.insert(hbuf.end(), buf.begin(), buf.end());

                if (headers.Parse(hbuf.data(), false)) {
                    auto it(headers.m.find("Content-Length"));
                    if (it != headers.m.end()) {
                        auto nTotal = atoi(it->second.c_str()) + 
                                      headers.bodyOffset;

                        nRest = nTotal - (hbuf.size() - 1);

                        if (nRest < 0) {
                            LogError("Invalid `Content-Length`!");
                            nRest = 0;
                        }
                    }
                    else {
                        if (headers.DetermineFinishedByStatusCode()) {
                            nRest = 0;
                        }
                        else if (headers.IsChunked()) {
                            chunked = true;

                            nRest = CountChunkRest(hbuf, headers.bodyOffset);

                            if (nRest == -1) {
                                LogError("Invalid chunk size!");
                                goto RECV_ERROR;
                            }
                        }
                    }

                    bHeadersParsed = true;
                }
                else {
                    hbuf.pop_back(); // 弹出结尾的 '\0'
                    continue; // 先不要输出
                }
            }
            // bHeadersParsed
            // nRest 没有设置表明既没有设置 Content-Length，也不是分段传输，
            // 那么应该是 Connection: close
            else if (nRest != -1) {
                // 注意“=”号，可能这个分段刚好占满了整个帧，而分段结束符
                // “\r\n”被切分到了下一个帧
                if (nRest == nReadBytes && chunked) {
                    goto NEXT_CHUNK;
                }

                if (nRest < nReadBytes) {
                    if (chunked) {
NEXT_CHUNK:
                        nRest = CountChunkRest(buf, nRest + 2);

                        if (nRest == -1) {
                            LogError("Invalid chunk size!");
                            goto RECV_ERROR;
                        }
                    }
                    else {
                        LogError("Junk data encountered!");
                        nRest = 0;
                    }
                }
                // nRest >= nReadBytes
                else {
                    nRest -= nReadBytes;
                }
            }

            auto &outbuf = hbuf.empty() ? buf : hbuf;
            auto rr = Write(m_bsocket, outbuf.data(), outbuf.size() - 1);

            if (rr != RR_ALIVE) {
                return false;
            }

            if (bHeadersParsed && !hbuf.empty()) {
                hbuf.clear();
            }

            if (nRest == 0) {
                if (m_headers.KeepAlive() && headers.KeepAlive()) {
                    return true;
                }
                else {
                    break;
                }
            }

            if (nRest == -1) {
                // Request received, continuing process
                if (headers.status_code < 200) {
                    return true;
                }
            }
        }
        else if (nReadBytes == SOCKET_ERROR) {
RECV_ERROR:
            auto fmt = __FUNC__ "recv() failed";
            LogError(WSAGetLastErrorMessage(fmt));

            ShutdownServerSocket();
            return false;
        }
    } while (nReadBytes > 0);

    LogInfo(__FUNC__ "Connection closed by server.");

    return ShutdownServerSocket();
}

int MyProxy::CountChunkRest(Buffer &buf, int offset) {
    if (offset >= static_cast<int>(buf.size() - 1)) {
        if (!ReadMore(buf, kExtraBytes)) {
            return -1;
        }
    }

    char *endptr;
    int nChunk = strtol(buf.data() + offset, &endptr, 16);

    // 段大小被拆开了
    if (*endptr == 0) {
        if (!ReadMore(buf, kExtraBytes)) {
            return -1;
        }

        nChunk = strtol(buf.data() + offset, &endptr, 16);
    }

    if (nChunk == 0) {
        if (!*endptr) {
            LogError("Invalid chunk size!");
        }

        return 0;
    }

    while (true) {
        endptr = strstr(endptr, "\r\n");
        if (endptr) {
            endptr += 2;

            int nRest = nChunk - (buf.data() + buf.size() - 1 - endptr);
            if (nRest < 0) {
                return CountChunkRest(buf, endptr - buf.data() + nChunk + 2);
            }

            return nRest;
        }

        if (!ReadMore(buf, kExtraBytes)) {
            return -1;
        }

        endptr = buf.data() + offset;
    }

    // 不可能来到这里
    assert(0);
    return -1;
}

bool MyProxy::ReadMore(Buffer &buf, int len) {
    auto nOldSize = buf.size(); // 已经添加一个'\0'所需的位置
    buf.resize(buf.size() + len);
    auto p = buf.data() + nOldSize - 1; // 原来'\0'所在的位置

    int nExtraRead = recv(m_ssocket, p, len, 0);
    if (nExtraRead <= 0) {
        return false;
    }

    *(p + nExtraRead) = 0;
    buf.resize(nOldSize + nExtraRead);

    return true;
}

u_long MyProxy::GetBestBufferSize(SOCKET sd) const {
    u_long nBufferSize = kBufferSize;
    if (ioctlsocket(sd, FIONREAD, &nBufferSize) != 0) {
        LogError(WSAGetLastErrorMessage(__FUNC__ "ioctlsocket() failed"));
        return -1;
    }

    return max(nBufferSize, kBufferSize);
}

MyProxy::RelayResult MyProxy::Write(SOCKET sd, const char *buf, size_t len) {
    size_t nSentBytes = 0;
    while (nSentBytes < len) {
        int n = send(sd, buf + nSentBytes, len - nSentBytes, 0);
        if (n > 0) {
            nSentBytes += n;
        }
        else if (n == SOCKET_ERROR) {
            LogError(WSAGetLastErrorMessage(__FUNC__ "send() failed"));
            return RR_ERROR;
        }
        else {
            // Browser or server closed connection before we could reply to
            // all the data it sent, so bomb out early.
            LogError("Connection unexpectedly dropped!");
            return RR_CLOSE;
        }
    }

    return RR_ALIVE;
}

void MyProxy::LogInfo(const string &msg) const {
    Log(msg, Logger::OL_INFO);
}

void MyProxy::LogError(const string &msg) const {
    Log(msg, Logger::OL_ERROR);
}

void MyProxy::Log(const string &msg, Logger::OutputLevel level) const {
    if (m_host.port > 0) {
        ostringstream ss;
        ss << m_host.name << ':' << m_host.port << '\n';
        ss << msg;

        Logger::Log(ss.str(), level);
    }
    else {
        Logger::Log(msg, level);
    }
}

//////////////////////////////////////////////////////////////////////////

bool MyProxy::Headers::Parse(const char *buf, bool browser) {
    this->Clear();

    const char *p = strstr(buf, "\r\n\r\n");
    if (p) {
        if (!browser && strncmp(buf, "HTTP/", 5) == 0) {
            this->status_code = atoi(buf + 9);
        }

        const char *b = strstr(buf, "\r\n") + 2;

        while (*b != '\r') {
            const char *e = strstr(b, "\r\n");
            const char *colon = strstr(b, ": ");
            if (!colon || colon > e) {
                colon = strchr(b, ':');
            }

            if (colon && colon + 2 < e) {
                string k(b, colon), v(colon + 2, e);
                this->m.emplace(make_pair(k, v));
            }

            b = e + 2;
        }

        this->bodyOffset = p + 4 - buf;
        return true;
    }

    return false;
}

void MyProxy::Headers::Clear() {
    this->status_code = 0;
    this->m.clear();
    this->bodyOffset = -1;
}

bool MyProxy::Headers::KeepAlive() const {
    auto it(m.find("Connection"));
    if (it != m.end()) {
        return ToLower(it->second) != "close";
    }

    // TODO: HTTP/1.1 默认是保持连接
    return true;
}

bool MyProxy::Headers::DetermineFinishedByStatusCode() const {
    if (status_code == 0) {
        return false;
    }

    if (status_code < 200 || status_code == 304) {
        return true;
    }

    return false;
}

bool MyProxy::Headers::IsChunked() const {
    auto it(this->m.find("Transfer-Encoding"));
    if (it != this->m.end()) {
        return ToLower(it->second) == "chunked";
    }

    return false;
}

//////////////////////////////////////////////////////////////////////////

string MyProxy::Host::GetFullName() const {
    ostringstream ss;
    ss << name << ':' << port;

    return ss.str();
}
