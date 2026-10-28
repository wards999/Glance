import CoreImage
import CoreImage.CIFilterBuiltins

/// The image adjustments (brightness, contrast and so on). Zero means "unchanged" for every value.
struct Adjustments: Equatable, Codable {
    var brightness: Double = 0     // -1...1
    var contrast: Double = 0       // -1...1
    var saturation: Double = 0     // -1...1
    var gamma: Double = 0          // -1...1, positive lifts midtones
    var sharpen: Double = 0        // 0...1
    var cyanRed: Double = 0        // -1...1
    var magentaGreen: Double = 0   // -1...1
    var yellowBlue: Double = 0     // -1...1
    var autoCorrect = false

    var isIdentity: Bool { self == Adjustments() }
}

/// Runs adjustments and edits through Core Image on the GPU. Use from one serial queue only:
/// the auto-correction filters are cached per picture and are not thread-safe.
final class ImageRenderer {
    static let shared = ImageRenderer()

    private let context = CIContext(options: [.cacheIntermediates: false])
    private var autoFilters: (key: String, filters: [CIFilter])?

    /// `autoKey` identifies the picture, so the auto-correction analysis is done once per picture
    /// and reused for every resolution of it.
    func apply(_ adjustments: Adjustments, to image: CGImage, autoKey: String) -> CGImage? {
        let input = CIImage(cgImage: image)
        let output = adjusted(input, adjustments, autoKey: autoKey)
        return render(output, like: image)
    }

    /// Adjustments, then rotation/flip, then crop: what gets written when saving.
    /// `crop` is in pixels of the rotated image, y down.
    func renderEdited(_ image: CGImage, adjustments: Adjustments, orientation: CGAffineTransform, crop: CGRect?, autoKey: String) -> CGImage? {
        var output = adjusted(CIImage(cgImage: image), adjustments, autoKey: autoKey)
        if orientation != .identity {
            output = output.transformed(by: orientation)
            output = output.transformed(by: CGAffineTransform(translationX: -output.extent.minX, y: -output.extent.minY))
        }
        if let crop {
            let height = output.extent.height
            let rect = CGRect(x: crop.minX, y: height - crop.maxY, width: crop.width, height: crop.height)
                .integral
                .intersection(output.extent)
            guard !rect.isEmpty else { return nil }
            output = output.cropped(to: rect).transformed(by: CGAffineTransform(translationX: -rect.minX, y: -rect.minY))
        }
        return render(output, like: image)
    }

    private func adjusted(_ input: CIImage, _ adjustments: Adjustments, autoKey: String) -> CIImage {
        guard !adjustments.isIdentity else { return input }
        var image = input

        if adjustments.autoCorrect {
            let filters: [CIFilter]
            if let cached = autoFilters, cached.key == autoKey {
                filters = cached.filters
            } else {
                filters = input.autoAdjustmentFilters(options: [.redEye: false])
                autoFilters = (autoKey, filters)
            }
            for filter in filters {
                filter.setValue(image, forKey: kCIInputImageKey)
                image = filter.outputImage ?? image
            }
        }

        if adjustments.cyanRed != 0 || adjustments.magentaGreen != 0 || adjustments.yellowBlue != 0 {
            let balance = CIFilter.colorMatrix()
            balance.inputImage = image
            balance.rVector = CIVector(x: 1 + adjustments.cyanRed * 0.3, y: 0, z: 0, w: 0)
            balance.gVector = CIVector(x: 0, y: 1 + adjustments.magentaGreen * 0.3, z: 0, w: 0)
            balance.bVector = CIVector(x: 0, y: 0, z: 1 + adjustments.yellowBlue * 0.3, w: 0)
            balance.aVector = CIVector(x: 0, y: 0, z: 0, w: 1)
            balance.biasVector = CIVector(x: 0, y: 0, z: 0, w: 0)
            image = balance.outputImage ?? image
        }

        if adjustments.brightness != 0 || adjustments.contrast != 0 || adjustments.saturation != 0 {
            let controls = CIFilter.colorControls()
            controls.inputImage = image
            controls.brightness = Float(adjustments.brightness * 0.25)
            controls.contrast = Float(1 + adjustments.contrast * 0.5)
            controls.saturation = Float(1 + adjustments.saturation)
            image = controls.outputImage ?? image
        }

        if adjustments.gamma != 0 {
            let gamma = CIFilter.gammaAdjust()
            gamma.inputImage = image
            gamma.power = Float(pow(2, -adjustments.gamma))
            image = gamma.outputImage ?? image
        }

        if adjustments.sharpen > 0 {
            let sharpen = CIFilter.sharpenLuminance()
            sharpen.inputImage = image
            sharpen.sharpness = Float(adjustments.sharpen * 2)
            sharpen.radius = 1.5
            image = sharpen.outputImage ?? image
        }

        return image.cropped(to: input.extent)
    }

    private func render(_ image: CIImage, like original: CGImage) -> CGImage? {
        let colorSpace = original.colorSpace.flatMap { $0.model == .rgb ? $0 : nil }
            ?? CGColorSpace(name: CGColorSpace.sRGB)!
        // Keep 16-bit sources 16-bit so saving doesn't throw away precision.
        let format: CIFormat = original.bitsPerComponent > 8 ? .RGBA16 : .RGBA8
        return context.createCGImage(image, from: image.extent, format: format, colorSpace: colorSpace)
    }
}
