// swift-tools-version:5.9
import PackageDescription

let package = Package(
    name: "Glance",
    platforms: [.macOS(.v13)],
    targets: [
        // mpv's headers, for video. libmpv itself is loaded at run time from the app's Frameworks
        // folder (scripts/fetch-mac-deps.sh, scripts/bundle.sh), only when a video is opened.
        .target(name: "CMpv", path: "Sources/CMpv"),
        .executableTarget(
            name: "Glance",
            dependencies: ["CMpv"],
            path: "Sources/Glance",
            linkerSettings: [
                // Where dlopen("@rpath/libmpv.2.dylib") and libmpv's own libraries are found.
                .unsafeFlags(["-Xlinker", "-rpath", "-Xlinker", "@executable_path/../Frameworks"]),
            ]
        ),
    ]
)
