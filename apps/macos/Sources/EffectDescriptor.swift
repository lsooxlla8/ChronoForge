import ChronoForgeCoreBridge
import Foundation

extension EffectNode {
    func coreDescriptor() -> CFEffectDescriptorV2 {
        let definition = kind.definition
        let normalizedValues = Array(values.prefix(definition.valueCount))
            + Array(repeating: 0, count: max(0, definition.valueCount - values.count))
        let normalizedOptions = Array(options.prefix(definition.optionCount))
            + Array(repeating: 0, count: max(0, definition.optionCount - options.count))
        return normalizedValues.withUnsafeBufferPointer { valueBuffer in
            normalizedOptions.withUnsafeBufferPointer { optionBuffer in
                cf_effect_descriptor_v2_make(
                    kind.rawValue,
                    min(max(amount, 0), 1),
                    randomSeed,
                    valueBuffer.baseAddress,
                    UInt32(definition.valueCount),
                    optionBuffer.baseAddress,
                    UInt32(definition.optionCount)
                )
            }
        }
    }
}
