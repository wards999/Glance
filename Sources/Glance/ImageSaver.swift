import CoreImage
import Foundation
import ImageIO
import UniformTypeIdentifiers

enum ImageSaver {
    enum SaveError: LocalizedError {
        case cannotRead
        case cannotWrite

        var errorDescription: String? {
            switch self {
            case .cannotRead: return "The image couldn't be read."
            case .cannotWrite: return "The image couldn't be written in that format."
            }
        }
    }

    static func canWrite(_ type: UTType) -> Bool {
        let writable = CGImageDestinationCopyTypeIdentifiers() as? [String] ?? []
        return writable.contains(type.identifier)
    }

    /// Applies a rotation/flip by rewriting only the orientation tag: no re-encode, no quality loss.
    /// Returns false when the format can't do that, so the caller falls back to re-encoding.
    static func rewriteOrientation(of source: URL, to destination: URL, applying transform: CGAffineTransform) throws -> Bool {
        guard let imageSource = CGImageSourceCreateWithURL(source as CFURL, nil),
              let type = CGImageSourceGetType(imageSource) else {
            throw SaveError.cannotRead
        }
        let properties = CGImageSourceCopyPropertiesAtIndex(imageSource, 0, nil) as? [CFString: Any]
        let raw = properties?[kCGImagePropertyOrientation] as? UInt32 ?? 1
        let current = CGImagePropertyOrientation(rawValue: raw) ?? .up
        guard let combined = compose(current, transform) else { return false }

        let temporary = temporaryURL(beside: destination)
        guard let destinationRef = CGImageDestinationCreateWithURL(temporary as CFURL, type, CGImageSourceGetCount(imageSource), nil) else {
            return false
        }
        let options: [CFString: Any] = [
            kCGImageDestinationMetadata: CGImageMetadataCreateMutable(),
            kCGImageDestinationMergeMetadata: true,
            kCGImageDestinationOrientation: combined.rawValue,
        ]
        guard CGImageDestinationCopyImageSource(destinationRef, imageSource, options as CFDictionary, nil) else {
            try? FileManager.default.removeItem(at: temporary)
            return false
        }
        try replace(destination, with: temporary)
        return true
    }

    /// Encodes `image`, carrying over EXIF/GPS/IPTC from `metadataSource`. Orientation is reset to
    /// "up" because the pixels are already upright.
    static func write(_ image: CGImage, to destination: URL, type: UTType, quality: Double, metadataSource: URL?, page: Int) throws {
        let temporary = temporaryURL(beside: destination)
        guard let destinationRef = CGImageDestinationCreateWithURL(temporary as CFURL, type.identifier as CFString, 1, nil) else {
            throw SaveError.cannotWrite
        }

        var properties: [CFString: Any] = [:]
        if let metadataSource,
           let source = CGImageSourceCreateWithURL(metadataSource as CFURL, nil),
           let original = CGImageSourceCopyPropertiesAtIndex(source, page, nil) as? [CFString: Any] {
            for key in [kCGImagePropertyExifDictionary, kCGImagePropertyGPSDictionary, kCGImagePropertyIPTCDictionary, kCGImagePropertyTIFFDictionary] {
                properties[key] = original[key]
            }
            if var tiff = properties[kCGImagePropertyTIFFDictionary] as? [CFString: Any] {
                tiff[kCGImagePropertyTIFFOrientation] = 1
                properties[kCGImagePropertyTIFFDictionary] = tiff
            }
            if var exif = properties[kCGImagePropertyExifDictionary] as? [CFString: Any] {
                // Stale after a crop.
                exif[kCGImagePropertyExifPixelXDimension] = nil
                exif[kCGImagePropertyExifPixelYDimension] = nil
                properties[kCGImagePropertyExifDictionary] = exif
            }
        }
        properties[kCGImagePropertyOrientation] = 1
        properties[kCGImageDestinationLossyCompressionQuality] = quality

        CGImageDestinationAddImage(destinationRef, image, properties as CFDictionary)
        guard CGImageDestinationFinalize(destinationRef) else {
            try? FileManager.default.removeItem(at: temporary)
            throw SaveError.cannotWrite
        }
        try replace(destination, with: temporary)
    }

    /// The orientation tag that shows the stored pixels as `current` followed by `transform`.
    /// Found by comparing transforms rather than a hand-written table of the eight EXIF cases.
    private static func compose(_ current: CGImagePropertyOrientation, _ transform: CGAffineTransform) -> CGImagePropertyOrientation? {
        let probe = CIImage(color: .black).cropped(to: CGRect(x: 0, y: 0, width: 2, height: 1))
        func linear(_ t: CGAffineTransform) -> [CGFloat] { [t.a, t.b, t.c, t.d].map { $0.rounded() } }
        let target = linear(probe.orientationTransform(for: current).concatenating(transform))
        let all: [CGImagePropertyOrientation] = [.up, .upMirrored, .down, .downMirrored, .left, .leftMirrored, .right, .rightMirrored]
        return all.first { linear(probe.orientationTransform(for: $0)) == target }
    }

    private static func temporaryURL(beside destination: URL) -> URL {
        destination.deletingLastPathComponent()
            .appendingPathComponent(".glance-\(UUID().uuidString)-\(destination.lastPathComponent)")
    }

    /// Swaps the finished file into place so a failed save never leaves a half-written image.
    private static func replace(_ destination: URL, with temporary: URL) throws {
        if FileManager.default.fileExists(atPath: destination.path) {
            _ = try FileManager.default.replaceItemAt(destination, withItemAt: temporary)
        } else {
            try FileManager.default.moveItem(at: temporary, to: destination)
        }
    }
}
