import Foundation

// Wire protocol shared with macassistd (see shared/include/macassist/
// protocol.hpp and PLAN.md section 6). Kept deliberately small and hand-
// written so the app has no build dependency on the C++ side.

enum Proto {
    static let version = 1

    // Socket path, matching DefaultSocketPath() on the daemon side. The
    // MACASSIST_SOCKET env override mirrors scripts/dev-run.sh, so the app
    // can point at a throwaway dev daemon.
    static var socketPath: String {
        if let override = ProcessInfo.processInfo.environment["MACASSIST_SOCKET"],
           !override.isEmpty {
            return override
        }
        let home = NSHomeDirectory()
        return home + "/Library/Application Support/MacAssist/daemon.sock"
    }
}

// A query request: {"v":1,"id":N,"type":"query","text":...,"stage":...}.
struct QueryRequest: Encodable {
    let v = Proto.version
    let id: Int
    let type = "query"
    let text: String
    let stage: String  // "instant" | "full"
    let limit = 50     // daemon defaults to 5 if omitted; we want the full set
}

// A Finder color tag attached to a file. `color` is the Finder label index
// (0 none, 1 gray, 2 green, 3 purple, 4 blue, 5 yellow, 6 red, 7 orange).
struct FileTag: Decodable, Hashable {
    let name: String
    let color: Int
}

// One result row from a `results` response.
struct FileHit: Decodable, Identifiable, Hashable {
    let fileId: Int
    let path: String
    let name: String
    let kind: String
    let score: Double
    let modifiedAt: Int
    // Absent on older daemons, so default to none rather than fail decoding.
    let tags: [FileTag]?

    var id: Int { fileId }

    enum CodingKeys: String, CodingKey {
        case fileId, path, name, kind, score, modifiedAt, tags
    }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        fileId = try c.decode(Int.self, forKey: .fileId)
        path = try c.decode(String.self, forKey: .path)
        name = try c.decode(String.self, forKey: .name)
        kind = try c.decode(String.self, forKey: .kind)
        score = try c.decode(Double.self, forKey: .score)
        modifiedAt = try c.decode(Int.self, forKey: .modifiedAt)
        tags = try c.decodeIfPresent([FileTag].self, forKey: .tags)
    }
}

// A `results` response frame. Other response types (error, pong) are
// decoded loosely via `type` before committing to this shape.
struct ResultsResponse: Decodable {
    let type: String
    let id: Int?
    let stage: String?
    let query: String?
    let items: [FileHit]?
}
