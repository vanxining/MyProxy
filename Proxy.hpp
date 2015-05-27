
#pragma once
#include "Logger.hpp"
#include "ws-util.h"

#include <vector>
#include <map>
#include <atomic>
using namespace std;

/// 代理对象
class MyProxy {
public:

    /// 构造函数
    MyProxy(SOCKET bsocket);

    /// 析构函数
    ~MyProxy();

    /// 处理来自浏览器的连接
    bool HandleBrowser();

    /// 打印 HTTP 头的第一行，包含 GET、POST 等信息
    void PrintRequest(Logger::OutputLevel level) const;

    /// 统计信息
    struct Statistics {
        /// 总的请求数
        atomic_int requests;

        /// 输入、输出字节数
        atomic_llong inBytes, outBytes;

        /// DNS 查询数
        atomic_int dnsQueries;

        /// DNS 缓存命中数
        atomic_int dnsCacheHit;
    };

    /// 获取统计信息
    static Statistics GetStatistics();

private:

    // 缓冲区类型
    typedef vector<char> Buffer;

    // 解析主机
    void SplitHost(const string &host_decl, int default_port);

    // 中转操作结果
    enum RelayResult {
        RR_ERROR, // 发生了错误
        RR_CLOSE, // 远程连接已关闭
        RR_ALIVE, // 没有发生任何错误，远程连接仍然保持
    };

    // 转发至服务器
    RelayResult HandleServer();
    RelayResult DoHandleServer();

    // 断开与服务器的连接
    bool ShutdownServerSocket();

    // 连接到服务器
    bool SetUpServerSocket();

    // 尝试使用 DNS 缓存的 IP 地址连接到服务器
    bool TryDNSCache();

    // 整理、发送来自浏览器的 HTTP 头部
    bool SendBrowserHeaders();

    // 中转 SSL 连接
    bool RelaySSLConnection();

    // 简单、机械的中转
    // 
    // 从 @a r 读，往 @a w 写
    RelayResult SimpleRelay(SOCKET r, SOCKET w, Buffer &buf);

    // 转发浏览器剩余的数据给服务器
    bool RelayToServer();

    // 取回服务器的回应给浏览器
    bool RelayToBrowser();

    // 计算指定 HTTP 分段的剩余字节数
    // 
    // @param buf 数据缓冲区
    // @param offset 表示分段大小的十六进制字符串在缓冲区中的偏移
    // @return 本段剩余未读的字节数。若为 -1，表明发生了读错误
    int CountChunkRest(Buffer &buf, int offset);

    // 额外读 @a len 字节的数据
    bool ReadMore(Buffer &buf, int len);

    // 获取最佳的缓冲区大小
    // 
    // @return 若返回 -1，表明发生了错误
    u_long GetBestBufferSize(SOCKET sd) const;

    // 往 SOCKET 写入数据
    RelayResult Write(SOCKET sd, const char *buf, size_t len);

private:

    void LogInfo(const string &msg) const;
    void LogError(const string &msg) const;

    void Log(const string &msg, Logger::OutputLevel level) const;

private:

    // 保存由浏览器发来的包含完整 HTTP 头部的一段数据
    // 可能不单单只是 HTTP 头部信息。
    Buffer m_vbuf;

    struct Host {
        void Clear() {
            this->name.clear();
            this->port = 0;
        }

        bool operator!=(const Host &other) {
            return this->name != other.name || this->port != other.port;
        }

        // 获取全名（加上端口）
        string GetFullName() const;

        string name;
        unsigned short port = 0;
    };

    Host m_host;

    // HTTP 头部
    struct Headers {
    public:

        // 解析头部
        // 
        // @param buf 必须保证以 0 结尾
        // @param browser 是否来自浏览器
        bool Parse(const char *buf, bool browser);

        // 清空内容
        void Clear();

        // 是否保持连接
        bool KeepAlive() const;

        // 根据状态码确定传输是否已然结束
        bool DetermineFinishedByStatusCode() const;

        // 是否分段
        bool IsChunked() const;

    public:

        int status_code = 0;

        map<string, string> m;
        int bodyOffset = -1;
    };

    Headers m_headers;

    // 统计信息
    static Statistics ms_stat;

    SOCKET m_bsocket;
    SOCKET m_ssocket;
};
