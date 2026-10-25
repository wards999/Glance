import Foundation
import ImageIO
import UniformTypeIdentifiers

/// What we know about one page of an image file without decoding its pixels.
struct ImageInfo {
    let url: URL
    /// Pixel size of the full-resolution page after EXIF orientation is applied.
    let fullSize: CGSize
    let pageCount: Int
    let isAnimated: Bool
    let typeIdentifier: String?
    let fileSize: Int64
    let properties: [CFString: Any]
    /// Set for PDFs: the document's title and which pages this screen shows.
    var pdf: PDFDetails? = nil

    var isRaw: Bool {
        typeIdentifier.flatMap(UTType.init)?.conforms(to: .rawImage) ?? false
    }

    var fullLongSide: Int {
        Int(max(fullSize.width, fullSize.height))
    }
}

struct DecodedImage {
    let image: CGImage
    let info: ImageInfo
    let page: Int

    var longSide: Int { max(image.width, image.height) }
    var cost: Int { image.bytesPerRow * image.height }
}

/// Thin wrapper over ImageIO. The speed comes from `CGImageSourceCreateThumbnailAtIndex`:
/// asking for a screen-sized image lets the JPEG/HEIF decoders skip most of the work
/// (JPEG decodes directly at 1/2, 1/4 or 1/8 scale), so a 100 MP photo opens about as fast as a 10 MP one.
enum ImageDecoder {
    /// Every extension ImageIO can decode on this Mac, including camera RAW, HEIC, AVIF and JPEG XL where supported.
    static let supportedExtensions: Set<String> = {
        let identifiers = CGImageSourceCopyTypeIdentifiers() as? [String] ?? []
        var extensions = Set<String>()
        for identifier in identifiers {
            guard let type = UTType(identifier) else { continue }
            for ext in type.tags[.filenameExtension] ?? [] {
                extensions.insert(ext.lowercased())
            }
        }
        return extensions
    }()

    static func isSupported(_ url: URL) -> Bool {
        supportedExtensions.contains(url.pathExtension.lowercased())
    }

    static func makeSource(_ url: URL) -> CGImageSource? {
        // Don't let ImageIO keep its own decoded copy; ImagePipeline does the caching.
        CGImageSourceCreateWithURL(url as CFURL, [kCGImageSourceShouldCache: false] as CFDictionary)
    }

    static func info(for url: URL, source: CGImageSource, page: Int) -> ImageInfo? {
        guard let properties = CGImageSourceCopyPropertiesAtIndex(source, page, nil) as? [CFString: Any] else {
            return nil
        }
        let width = properties[kCGImagePropertyPixelWidth] as? Int ?? 0
        let height = properties[kCGImagePropertyPixelHeight] as? Int ?? 0
        guard width > 0, height > 0 else { return nil }

        // EXIF orientations 5–8 are rotated by 90°, so width and height swap on screen.
        let orientation = properties[kCGImagePropertyOrientation] as? UInt32 ?? 1
        let swapsAxes = (5...8).contains(orientation)
        let fullSize = swapsAxes ? CGSize(width: height, height: width) : CGSize(width: width, height: height)

        let count = CGImageSourceGetCount(source)
        let type = CGImageSourceGetType(source) as String?
        let animatedTypes: Set<String> = ["com.compuserve.gif", "public.png", "org.webmproject.webp", "public.heics"]
        let isAnimated = count > 1 && type.map { animatedTypes.contains($0) } == true
        let fileSize = Int64((try? url.resourceValues(forKeys: [.fileSizeKey]))?.fileSize ?? 0)

        return ImageInfo(
            url: url,
            fullSize: fullSize,
            pageCount: count,
            isAnimated: isAnimated,
            typeIdentifier: type,
            fileSize: fileSize,
            properties: properties
        )
    }

    /// Decodes `page` with its longest side at most `maxPixelSize`, rotated per EXIF and fully
    /// decoded up front, so handing it to Core Animation never stalls the main thread.
    static func decode(source: CGImageSource, page: Int, maxPixelSize: Int, preferEmbedded: Bool = false) -> CGImage? {
        let options: [CFString: Any] = [
            // RAW files carry a large embedded JPEG; using it skips a full RAW develop.
            (preferEmbedded ? kCGImageSourceCreateThumbnailFromImageIfAbsent : kCGImageSourceCreateThumbnailFromImageAlways): true,
            kCGImageSourceCreateThumbnailWithTransform: true,
            kCGImageSourceThumbnailMaxPixelSize: maxPixelSize,
            kCGImageSourceShouldCacheImmediately: true,
        ]
        return CGImageSourceCreateThumbnailAtIndex(source, page, options as CFDictionary)
    }

    /// The thumbnail stored inside the file (EXIF thumbnail, RAW preview), or nil.
    /// Never touches the main image data, so it's nearly free.
    static func embeddedPreview(source: CGImageSource, page: Int) -> CGImage? {
        let options: [CFString: Any] = [
            kCGImageSourceCreateThumbnailWithTransform: true,
            kCGImageSourceShouldCacheImmediately: true,
        ]
        return CGImageSourceCreateThumbnailAtIndex(source, page, options as CFDictionary)
    }

    /// Full-resolution, EXIF-oriented pixels. Used for crop, save, print and copy.
    /// For a PDF, `page` is the screen (page or spread) index under `layout`.
    static func decodeFull(_ url: URL, page: Int, layout: PDFLayout = PDFLayout(), password: String? = nil) -> CGImage? {
        if PDFRenderer.isPDF(url) {
            return PDFRenderer.decode(url, index: page, size: .longSide(Int.max), layout: layout, password: password)?.image
        }
        guard let source = makeSource(url), let info = info(for: url, source: source, page: page) else { return nil }
        return decode(source: source, page: page, maxPixelSize: info.fullLongSide)
    }
}
