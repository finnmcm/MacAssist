import Foundation

// A tiny synchronous client for the daemon's Unix socket. Each call opens
// a fresh connection, sends one length-prefixed JSON frame, and reads one
// frame back — matching ReadFrame/WriteFrame in shared/src/frame.cpp
// (4-byte big-endian length, then UTF-8 JSON, 16 MiB cap). Callers run it
// off the main thread; the daemon serves one connection at a time.
struct DaemonClient {
    let socketPath: String

    enum ClientError: Error, CustomStringConvertible {
        case connect(String)
        case io(String)
        case decode(String)

        var description: String {
            switch self {
            case .connect(let m): return "connect: \(m)"
            case .io(let m): return "io: \(m)"
            case .decode(let m): return "decode: \(m)"
            }
        }
    }

    private static let maxFrameSize: UInt32 = 16 * 1024 * 1024

    // Runs a full-stage query and returns the ranked hits.
    func query(_ text: String, id: Int, stage: String = "full") throws -> [FileHit] {
        let payload = try JSONEncoder().encode(
            QueryRequest(id: id, text: text, stage: stage))
        let responseData = try roundTrip(payload)
        let resp = try JSONDecoder().decode(ResultsResponse.self, from: responseData)
        if resp.type == "error" {
            let msg = (try? JSONSerialization.jsonObject(with: responseData))
                .flatMap { ($0 as? [String: Any])?["message"] as? String }
            throw ClientError.decode(msg ?? "daemon returned an error")
        }
        guard resp.type == "results" else {
            throw ClientError.decode("unexpected response type: \(resp.type)")
        }
        return resp.items ?? []
    }

    // MARK: - connection

    private func roundTrip(_ payload: Data) throws -> Data {
        let fd = socket(AF_UNIX, SOCK_STREAM, 0)
        guard fd >= 0 else { throw ClientError.connect(errnoString()) }
        defer { close(fd) }

        var addr = sockaddr_un()
        addr.sun_family = sa_family_t(AF_UNIX)
        let pathBytes = socketPath.utf8CString
        let capacity = MemoryLayout.size(ofValue: addr.sun_path)
        guard pathBytes.count <= capacity else {
            throw ClientError.connect("socket path too long")
        }
        withUnsafeMutablePointer(to: &addr.sun_path) { raw in
            raw.withMemoryRebound(to: CChar.self, capacity: capacity) { dst in
                pathBytes.withUnsafeBufferPointer { src in
                    dst.update(from: src.baseAddress!, count: src.count)
                }
            }
        }
        let rc = withUnsafePointer(to: &addr) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                connect(fd, $0, socklen_t(MemoryLayout<sockaddr_un>.size))
            }
        }
        guard rc == 0 else { throw ClientError.connect(errnoString()) }

        try writeFrame(fd, payload)
        return try readFrame(fd)
    }

    private func writeFrame(_ fd: Int32, _ payload: Data) throws {
        let n = UInt32(payload.count)
        var frame = Data([
            UInt8((n >> 24) & 0xff), UInt8((n >> 16) & 0xff),
            UInt8((n >> 8) & 0xff), UInt8(n & 0xff),
        ])
        frame.append(payload)
        try writeAll(fd, frame)
    }

    private func readFrame(_ fd: Int32) throws -> Data {
        let header = try readN(fd, 4)
        let length = (UInt32(header[0]) << 24) | (UInt32(header[1]) << 16)
            | (UInt32(header[2]) << 8) | UInt32(header[3])
        guard length <= Self.maxFrameSize else {
            throw ClientError.io("frame too large: \(length)")
        }
        return length == 0 ? Data() : Data(try readN(fd, Int(length)))
    }

    private func writeAll(_ fd: Int32, _ data: Data) throws {
        try data.withUnsafeBytes { (raw: UnsafeRawBufferPointer) in
            var p = raw.baseAddress!
            var remaining = raw.count
            while remaining > 0 {
                let n = write(fd, p, remaining)
                if n < 0 {
                    if errno == EINTR { continue }
                    throw ClientError.io(errnoString())
                }
                p = p.advanced(by: n)
                remaining -= n
            }
        }
    }

    private func readN(_ fd: Int32, _ count: Int) throws -> [UInt8] {
        var buf = [UInt8](repeating: 0, count: count)
        var got = 0
        try buf.withUnsafeMutableBytes { (raw: UnsafeMutableRawBufferPointer) in
            let base = raw.baseAddress!
            while got < count {
                let n = read(fd, base.advanced(by: got), count - got)
                if n == 0 { throw ClientError.io("peer closed") }
                if n < 0 {
                    if errno == EINTR { continue }
                    throw ClientError.io(errnoString())
                }
                got += n
            }
        }
        return buf
    }
}

private func errnoString() -> String {
    String(cString: strerror(errno))
}
