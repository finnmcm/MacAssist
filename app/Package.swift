// swift-tools-version:5.9
import PackageDescription

// The MacAssist UI: a menu-bar-resident, non-activating search panel that
// talks to macassistd over the Unix domain socket. Built as a standalone
// SwiftPM executable for Phase 1; the eventual Swift<->C++ interop work
// (Phase 3, AFM + CoreML in the daemon) is what forces the move to an
// Xcode workspace, and nothing here needs that yet.
let package = Package(
    name: "MacAssist",
    platforms: [.macOS(.v14)],
    targets: [
        .executableTarget(
            name: "MacAssist",
            path: "Sources/MacAssist"
        )
    ]
)
