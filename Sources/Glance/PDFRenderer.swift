import CoreGraphics
import Foundation

/// How a PDF's pages are put on screen.
enum PDFPageMode: Int, CaseIterable {
    /// Magazine-like documents get a cover then spreads; everything else single pages.
    case automatic = 0
    case single
    case spreads
    /// Page 1 alone (the cover), then 2–3, 4–5, …
    case spreadsWithCover

    var title: String {
        switch self {
        case .automatic: return "Automatic"
        case .single: return "Single Page"
        case .spreads: return "Two Pages"
        case .spreadsWithCover: return "Two Pages with Cover"
        }
    }
}

struct PDFLayout: Hashable {
    var mode = PDFPageMode.single
    var rightToLeft = false
}

/// What the title bar and info panel show for a PDF.
struct PDFDetails {
    let title: String?
    let author: String?
    let pageCount: Int
    /// 1-based page numbers on this screen, left to right in reading order.
    let pages: [Int]

    var pagesText: String {
        let shown = pages.count == 1 ? "Page \(pages[0])" : "Pages \(pages[0])–\(pages[pages.count - 1])"
        return "\(shown) of \(pageCount)"
    }
}

/// Renders PDF pages with Core Graphics, which every image already loads, so PDF support adds
/// nothing to launch. Each screen (a page or a two-page spread) is one bitmap, so the image
/// pipeline caches and prefetches it like a photo.
enum PDFRenderer {
    /// Vector pages have no native pixel size; "full resolution" is 4 pixels per point (288 dpi),
    /// capped so a poster-sized page stays within a sane bitmap.
    static let pixelsPerPoint: CGFloat = 4
    static let maxFullLongSide: CGFloat = 16384

    static func isPDF(_ url: URL) -> Bool {
        url.pathExtension.caseInsensitiveCompare("pdf") == .orderedSame
    }

    /// Opens a document per call: CGPDFDocument isn't safe to draw from several threads at once.
    static func open(_ url: URL, password: String?) -> CGPDFDocument? {
        guard let document = CGPDFDocument(url as CFURL) else { return nil }
        if !document.isUnlocked {
            guard let password, document.unlockWithPassword(password) else { return nil }
        }
        return document
    }

    /// True when the file is a PDF that won't open without a (different) password.
    static func needsPassword(_ url: URL, password: String?) -> Bool {
        guard let document = CGPDFDocument(url as CFURL), !document.isUnlocked else { return false }
        guard let password else { return true }
        return !document.unlockWithPassword(password)
    }

    /// Which pages share each screen.
    static func spreads(for document: CGPDFDocument, layout: PDFLayout) -> [[Int]] {
        let count = document.numberOfPages
        guard count > 0 else { return [] }
        var mode = layout.mode
        if mode == .automatic {
            mode = looksLikeMagazine(document) ? .spreadsWithCover : .single
        }
        guard mode != .single else { return (1...count).map { [$0] } }

        var result: [[Int]] = []
        var page = 1
        if mode == .spreadsWithCover {
            result.append([1])
            page = 2
        }
        while page <= count {
            // A landscape page is usually a spread already, so it stands alone.
            if page == count || isLandscape(document, page) || isLandscape(document, page + 1) {
                result.append([page])
                page += 1
            } else {
                result.append([page, page + 1])
                page += 2
            }
        }
        return result
    }

    /// Spreads for `url` without rendering anything, e.g. to keep your place when the layout changes.
    static func spreads(for url: URL, layout: PDFLayout, password: String?) -> [[Int]] {
        guard let document = open(url, password: password) else { return [] }
        return spreads(for: document, layout: layout)
    }

    /// Renders screen `index` just big enough for `size`.
    static func decode(_ url: URL, index: Int, size: ImagePipeline.DecodeSize, layout: PDFLayout, password: String?) -> DecodedImage? {
        guard let document = open(url, password: password) else { return nil }
        let screens = spreads(for: document, layout: layout)
        guard screens.indices.contains(index) else { return nil }
        let pages = screens[index]

        let points = spreadSize(document, pages: pages)
        guard points.width > 0, points.height > 0 else { return nil }
        let fullScale = min(pixelsPerPoint, maxFullLongSide / max(points.width, points.height))
        let fullSize = CGSize(width: (points.width * fullScale).rounded(), height: (points.height * fullScale).rounded())
        let fileSize = Int64((try? url.resourceValues(forKeys: [.fileSizeKey]))?.fileSize ?? 0)
        let info = ImageInfo(
            url: url,
            fullSize: fullSize,
            pageCount: screens.count,
            isAnimated: false,
            typeIdentifier: "com.adobe.pdf",
            fileSize: fileSize,
            properties: [:],
            pdf: PDFDetails(
                title: string(document.info, "Title"),
                author: string(document.info, "Author"),
                pageCount: document.numberOfPages,
                pages: pages
            )
        )

        // Unlike JPEG, rendering costs the same per pixel at any size, so render exactly what's needed.
        let longSide = ImagePipeline.neededLongSide(for: size, info: info)
        guard let image = render(document, pages: pages, longSide: longSide, rightToLeft: layout.rightToLeft) else { return nil }
        return DecodedImage(image: image, info: info, page: index)
    }

    // MARK: - Private

    /// Mostly portrait pages of a similar size, and enough of them to be a publication.
    private static func looksLikeMagazine(_ document: CGPDFDocument) -> Bool {
        let count = document.numberOfPages
        guard count >= 4 else { return false }
        let sample = Array(1...min(count, 20))
        let portrait = sample.filter { !isLandscape(document, $0) }.count
        return Double(portrait) / Double(sample.count) >= 0.8
    }

    private static func isLandscape(_ document: CGPDFDocument, _ number: Int) -> Bool {
        guard let page = document.page(at: number) else { return false }
        let size = displaySize(page)
        return size.width > size.height
    }

    /// Page size in points as it should appear, after the page's own /Rotate.
    private static func displaySize(_ page: CGPDFPage) -> CGSize {
        let box = page.getBoxRect(.cropBox)
        let rotation = ((Int(page.rotationAngle) % 360) + 360) % 360
        return rotation == 90 || rotation == 270 ? CGSize(width: box.height, height: box.width) : box.size
    }

    /// Pages side by side, all scaled to the tallest page's height.
    private static func spreadSize(_ document: CGPDFDocument, pages: [Int]) -> CGSize {
        let sizes = pages.compactMap { document.page(at: $0).map(displaySize) }
        let height = sizes.map(\.height).max() ?? 0
        let width = sizes.reduce(0) { $0 + ($1.height > 0 ? $1.width * height / $1.height : 0) }
        return CGSize(width: width, height: height)
    }

    private static func render(_ document: CGPDFDocument, pages: [Int], longSide: Int, rightToLeft: Bool) -> CGImage? {
        let points = spreadSize(document, pages: pages)
        let scale = CGFloat(max(longSide, 1)) / max(points.width, points.height)
        let width = max(Int((points.width * scale).rounded()), 1)
        let height = max(Int((points.height * scale).rounded()), 1)
        // BGRA premultiplied: the layout the GPU takes directly, so display never converts it.
        guard let space = CGColorSpace(name: CGColorSpace.sRGB),
              let context = CGContext(
                data: nil, width: width, height: height, bitsPerComponent: 8, bytesPerRow: 0, space: space,
                bitmapInfo: CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue
              ) else { return nil }

        // Paper is white even when the PDF leaves it transparent.
        context.setFillColor(CGColor(gray: 1, alpha: 1))
        context.fill(CGRect(x: 0, y: 0, width: width, height: height))
        context.interpolationQuality = .high

        var x: CGFloat = 0
        for number in rightToLeft ? Array(pages.reversed()) : pages {
            guard let page = document.page(at: number) else { continue }
            let size = displaySize(page)
            let pageScale = CGFloat(height) / size.height
            let rect = CGRect(x: x, y: 0, width: size.width * pageScale, height: CGFloat(height))
            context.saveGState()
            context.clip(to: rect)
            draw(page, in: rect, scale: pageScale, context: context)
            context.restoreGState()
            x += rect.width
        }
        return context.makeImage()
    }

    /// Like CGPDFPage.getDrawingTransform, which can't scale up, so it's no use for sharp zoom.
    private static func draw(_ page: CGPDFPage, in rect: CGRect, scale: CGFloat, context: CGContext) {
        let box = page.getBoxRect(.cropBox)
        let rotation = ((Int(page.rotationAngle) % 360) + 360) % 360
        context.translateBy(x: rect.minX, y: rect.minY)
        context.scaleBy(x: scale, y: scale)
        // /Rotate turns the page clockwise for display.
        switch rotation {
        case 90:
            context.rotate(by: -.pi / 2)
            context.translateBy(x: -box.width, y: 0)
        case 180:
            context.rotate(by: .pi)
            context.translateBy(x: -box.width, y: -box.height)
        case 270:
            context.rotate(by: .pi / 2)
            context.translateBy(x: 0, y: -box.height)
        default:
            break
        }
        context.translateBy(x: -box.minX, y: -box.minY)
        context.drawPDFPage(page)
    }

    private static func string(_ dictionary: CGPDFDictionaryRef?, _ key: String) -> String? {
        guard let dictionary else { return nil }
        var value: CGPDFStringRef?
        guard CGPDFDictionaryGetString(dictionary, key, &value), let value,
              let text = CGPDFStringCopyTextString(value) as String?, !text.isEmpty else { return nil }
        return text
    }
}
