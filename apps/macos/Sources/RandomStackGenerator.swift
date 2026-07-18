import Foundation

struct SplitMix64: RandomNumberGenerator, Sendable {
    private var state: UInt64

    init(seed: UInt64) { state = seed }

    mutating func next() -> UInt64 {
        state &+= 0x9E3779B97F4A7C15
        var value = state
        value = (value ^ (value >> 30)) &* 0xBF58476D1CE4E5B9
        value = (value ^ (value >> 27)) &* 0x94D049BB133111EB
        return value ^ (value >> 31)
    }

    mutating func unit() -> Double {
        Double(next() >> 11) / Double(UInt64(1) << 53)
    }

    mutating func integer(in range: ClosedRange<Int>) -> Int {
        guard range.lowerBound < range.upperBound else { return range.lowerBound }
        let width = UInt64(range.upperBound - range.lowerBound + 1)
        return range.lowerBound + Int(next() % width)
    }

    mutating func chance(_ probability: Double) -> Bool { unit() < probability }
}

extension RandomFloatDistribution {
    func sample(using random: inout SplitMix64) -> Float {
        switch self {
        case .uniform(let range):
            return range.lowerBound + Float(random.unit()) * (range.upperBound - range.lowerBound)
        case .triangular(let range, let preferred):
            let lower = Double(range.lowerBound), upper = Double(range.upperBound)
            let mode = Double(min(max(preferred, range.lowerBound), range.upperBound))
            let fraction = (mode - lower) / max(Double.leastNonzeroMagnitude, upper - lower)
            let sample = random.unit()
            if sample < fraction {
                return Float(lower + sqrt(sample * (upper - lower) * (mode - lower)))
            }
            return Float(upper - sqrt((1 - sample) * (upper - lower) * (upper - mode)))
        case .logarithmic(let range):
            let lower = max(range.lowerBound, Float.leastNonzeroMagnitude)
            return exp(log(lower) + Float(random.unit()) * (log(range.upperBound) - log(lower)))
        case .signedMagnitude(let range, let deadZone):
            let magnitude = max(deadZone, RandomFloatDistribution.logarithmic(range).sample(using: &random))
            return random.chance(0.5) ? magnitude : -magnitude
        case .fixed(let value):
            return value
        }
    }
}

enum RandomStackGenerator {
    static func generate(
        mediaPool: [DecodedProxy],
        primaryMediaID: UUID?,
        seed: UInt64
    ) throws -> [EffectNode] {
        var random = SplitMix64(seed: seed)
        let targetCount: Int
        let lengthRoll = random.unit()
        if lengthRoll < 0.35 { targetCount = 1 }
        else if lengthRoll < 0.80 { targetCount = 2 }
        else { targetCount = 3 }

        let driverIDs = mediaPool.map(\.id).filter { $0 != primaryMediaID }
        let eligible = EffectRegistry.definitions.filter { definition in
            definition.isAddable && definition.randomization != nil &&
                (definition.inputArity == .one || !driverIDs.isEmpty)
        }
        guard !eligible.isEmpty else {
            throw CocoaError(.validationMissingMandatoryProperty, userInfo: [
                NSLocalizedDescriptionKey: "No effects are available for Random Stack."
            ])
        }

        var selected: [EffectDefinition] = []
        var attempts = 0
        while selected.count < targetCount && attempts < 200 {
            attempts += 1
            let candidate = eligible[random.integer(in: 0...(eligible.count - 1))]
            if candidate.costClass == .global && selected.contains(where: { $0.costClass == .global }) { continue }
            if selected.contains(where: { $0.kind == candidate.kind }) && !random.chance(0.12) { continue }
            if candidate.kind == .seamlessLoop && selected.contains(where: { $0.kind == .seamlessLoop }) { continue }
            selected.append(candidate)
        }
        guard selected.count == targetCount else {
            throw CocoaError(.validationMultipleErrors, userInfo: [
                NSLocalizedDescriptionKey: "Could not build a compatible random effect stack."
            ])
        }

        var nodes = selected.map { definition -> EffectNode in
            var node = definition.defaultNode()
            node.id = deterministicUUID(using: &random)
            node.randomSeed = random.next()
            node.amount = definition.randomization!.amount.sample(using: &random)
            if definition.inputArity == .two {
                node.driverMediaID = driverIDs[random.integer(in: 0...(driverIDs.count - 1))]
            }
            randomize(&node, profile: definition.randomization!, mediaPool: mediaPool, using: &random)
            if !node.supportsAmount { node.amount = 1 }
            return node
        }
        nodes.sort { left, right in left.kind != .seamlessLoop && right.kind == .seamlessLoop }
        for index in nodes.indices {
            nodes[index].inputNodeID = index == 0 ? nil : nodes[index - 1].id
        }
        try validate(nodes, mediaPool: mediaPool)
        return nodes
    }

    private static func randomize(
        _ node: inout EffectNode,
        profile: RandomizationProfile,
        mediaPool: [DecodedProxy],
        using random: inout SplitMix64
    ) {
        switch profile.identifier {
        case "tensor-swap":
            node.options = [Int32(random.integer(in: 0...1)), 1]
        case "time-shift":
            node.values[0] = RandomFloatDistribution.signedMagnitude(5...100, deadZone: 5).sample(using: &random)
            node.options = [weighted([0, 0, 0, 1, 2, 3], using: &random), Int32(random.integer(in: 0...2))]
        case "polar":
            node.values = [
                .triangular(0.15...0.85, preferred: 0.5, using: &random),
                .triangular(0.15...0.85, preferred: 0.5, using: &random),
                RandomFloatDistribution.signedMagnitude(0.03...0.65, deadZone: 0.03).sample(using: &random),
                RandomFloatDistribution.signedMagnitude(0.1...2.5, deadZone: 0.1).sample(using: &random),
                RandomFloatDistribution.uniform(-180...180).sample(using: &random),
            ]
            node.options = [
                Int32(random.integer(in: 0...2)),
                Int32(random.integer(in: 0...2)),
                random.chance(0.65) ? 1 : 0,
            ]
        case "pixel-sort":
            node.values[0] = .triangular(0.05...0.9, preferred: 0.35, using: &random)
            node.values[1] = RandomFloatDistribution.uniform(-180...180).sample(using: &random)
            node.options = [Int32(random.integer(in: 0...2)), Int32(random.integer(in: 0...3))]
        case "spectral":
            let rotation = random.chance(0.65)
            node.values[0] = rotation
                ? RandomFloatDistribution.signedMagnitude(5...120, deadZone: 5).sample(using: &random) : 0
            node.options = [Int32(random.integer(in: 0...2)), random.chance(0.35) ? 1 : 0, 1, rotation ? 1 : 0]
        case "space-time-map":
            var axes: [Int32] = [0, 1, 2]
            axes.shuffle(using: &random)
            for index in axes.indices where random.chance(0.55) { axes[index] += 3 }
            if !axes.contains(where: { $0 >= 3 }) { axes[random.integer(in: 0...2)] += 3 }
            node.options = axes + [Int32(random.integer(in: 0...2))]
        case "tensor-displacement":
            node.values = [
                RandomFloatDistribution.signedMagnitude(2...120, deadZone: 2).sample(using: &random),
                RandomFloatDistribution.signedMagnitude(4...400, deadZone: 4).sample(using: &random),
                RandomFloatDistribution.signedMagnitude(4...400, deadZone: 4).sample(using: &random),
            ]
            node.options = [Int32(random.integer(in: 0...4)), Int32(random.integer(in: 0...1)), Int32(random.integer(in: 0...2))]
        case "optical-flow":
            node.values = [
                RandomFloatDistribution.logarithmic(0.005...0.25).sample(using: &random),
                RandomFloatDistribution.signedMagnitude(1...30, deadZone: 1).sample(using: &random),
                RandomFloatDistribution.uniform(-180...180).sample(using: &random),
                .triangular(20...180, preferred: 90, using: &random),
            ]
            node.options = [Int32(random.integer(in: 0...2))]
        case "feedback":
            node.values = [
                RandomFloatDistribution.logarithmic(1...90).sample(using: &random),
                .triangular(0.05...0.65, preferred: 0.25, using: &random),
                RandomFloatDistribution.logarithmic(1...90).sample(using: &random),
                .triangular(0.02...0.5, preferred: 0.15, using: &random),
            ]
            node.options = [Int32(random.integer(in: 0...5))]
        case "datamosh":
            let trigger = Int32(random.integer(in: 0...2))
            node.values = [
                .triangular(0.05...0.8, preferred: 0.25, using: &random),
                RandomFloatDistribution.logarithmic(2...180).sample(using: &random),
                RandomFloatDistribution.logarithmic(0.002...0.25).sample(using: &random),
            ]
            node.options = [Int32(random.integer(in: 0...2)), trigger, trigger == 1 && random.chance(0.5) ? 1 : 0]
        case "seamless":
            let sourceFrames = mediaPool.first?.tensor.frames ?? 30
            node.values = [Float(random.integer(in: 2...max(2, min(60, sourceFrames / 2)))),
                           .triangular(0.03...0.3, preferred: 0.12, using: &random)]
            node.options = [weighted([0, 0, 1, 1, 2, 3, 4, 4], using: &random)]
        case "rgb-time-slip":
            let stationary = random.integer(in: 0...2)
            var offsets = [Float](repeating: 0, count: 3)
            let moving = (0...2).filter { $0 != stationary }
            offsets[stationary] = RandomFloatDistribution.uniform(-2...2).sample(using: &random)
            offsets[moving[0]] = -RandomFloatDistribution.logarithmic(4...180).sample(using: &random)
            offsets[moving[1]] = RandomFloatDistribution.logarithmic(4...180).sample(using: &random)
            if random.chance(0.5) { offsets.swapAt(moving[0], moving[1]) }
            node.values = offsets + [RandomFloatDistribution.signedMagnitude(2...120, deadZone: 2).sample(using: &random)]
            node.options = [Int32(random.integer(in: 0...2)), Int32(random.integer(in: 0...2))]
        case "horizontal-sync-loss":
            node.values = [
                .triangular(0.05...0.85, preferred: 0.25, using: &random),
                RandomFloatDistribution.logarithmic(0.005...0.75).sample(using: &random),
                RandomFloatDistribution.signedMagnitude(0.05...4, deadZone: 0.05).sample(using: &random),
                .triangular(0.08...0.9, preferred: 0.35, using: &random),
            ]
            node.options = [weighted([0, 0, 0, 1, 2], using: &random), Int32(random.integer(in: 0...2)), Int32(random.integer(in: 0...1))]
        case "chroma-carrier-drift":
            node.values = [
                RandomFloatDistribution.signedMagnitude(2...240, deadZone: 2).sample(using: &random),
                RandomFloatDistribution.signedMagnitude(1...160, deadZone: 1).sample(using: &random),
                RandomFloatDistribution.signedMagnitude(0.5...60, deadZone: 0.5).sample(using: &random),
                RandomFloatDistribution.logarithmic(0.5...60).sample(using: &random),
            ]
            node.options = [weighted([0, 1, 1, 2], using: &random), Int32(random.integer(in: 0...2))]
        case "stride-error":
            node.values = [
                RandomFloatDistribution.signedMagnitude(0.005...0.45, deadZone: 0.005).sample(using: &random),
                RandomFloatDistribution.signedMagnitude(0.005...0.9, deadZone: 0.005).sample(using: &random),
                RandomFloatDistribution.signedMagnitude(0.001...0.25, deadZone: 0.001).sample(using: &random),
            ]
            node.options = [weighted([0, 0, 1, 2], using: &random), Int32(random.integer(in: 0...1))]
        case "block-address-corruption":
            node.values = [
                RandomFloatDistribution.logarithmic(0.005...0.8).sample(using: &random),
                .triangular(0.08...0.9, preferred: 0.35, using: &random),
                RandomFloatDistribution.logarithmic(0.5...120).sample(using: &random),
                RandomFloatDistribution.logarithmic(1...90).sample(using: &random),
            ]
            node.options = [Int32(random.integer(in: 0...3)), Int32(random.integer(in: 0...2))]
        case "bitplane-forge":
            let bits = random.integer(in: 3...16)
            let lowest = random.integer(in: 0...(bits - 1))
            let highest = random.integer(in: lowest...(bits - 1))
            let mask = ((1 << (highest - lowest + 1)) - 1) << lowest
            node.values = [Float(bits), Float(mask), Float(random.integer(in: -15...15))]
            node.options = [weighted([0, 1, 2, 2, 3], using: &random), Int32(random.integer(in: 0...5))]
        case "signal-weave":
            node.values = [
                RandomFloatDistribution.logarithmic(0.005...0.8).sample(using: &random),
                RandomFloatDistribution.signedMagnitude(0.02...8, deadZone: 0.02).sample(using: &random),
                .triangular(0...0.7, preferred: 0.12, using: &random),
                RandomFloatDistribution.signedMagnitude(1...120, deadZone: 1).sample(using: &random),
            ]
            node.options = [Int32(random.integer(in: 0...3)), weighted([0, 0, 1, 1, 2], using: &random)]
        case "block-graft":
            node.values = [
                RandomFloatDistribution.logarithmic(0.005...0.8).sample(using: &random),
                .triangular(0.08...0.9, preferred: 0.35, using: &random),
                RandomFloatDistribution.logarithmic(1...90).sample(using: &random),
                RandomFloatDistribution.signedMagnitude(1...120, deadZone: 1).sample(using: &random),
            ]
            node.options = [weighted([0, 0, 1, 2, 3, 4], using: &random), weighted([0, 0, 1, 1, 2], using: &random)]
        case "channel-transplant":
            var mappings = [Int32(random.integer(in: 0...1)), Int32(random.integer(in: 0...1)), Int32(random.integer(in: 0...1))]
            if !mappings.contains(1) { mappings[random.integer(in: 0...2)] = 1 }
            node.values = [
                RandomFloatDistribution.signedMagnitude(1...120, deadZone: 1).sample(using: &random),
                RandomFloatDistribution.signedMagnitude(1...320, deadZone: 1).sample(using: &random),
                RandomFloatDistribution.signedMagnitude(1...240, deadZone: 1).sample(using: &random),
            ]
            node.options = mappings + [Int32(random.integer(in: 0...1)), weighted([0, 0, 1, 1, 2], using: &random)]
        default:
            break
        }
    }

    private static func validate(_ nodes: [EffectNode], mediaPool: [DecodedProxy]) throws {
        guard nodes.count...3 ~= nodes.count,
              nodes.filter({ $0.kind.definition.costClass == .global }).count <= 1,
              nodes.dropLast().allSatisfy({ $0.kind != .seamlessLoop }),
              nodes.allSatisfy({ $0.amount == 1 || $0.supportsAmount }),
              nodes.allSatisfy({ !$0.kind.requiresDriver || $0.driverMediaID.map { id in mediaPool.contains(where: { $0.id == id }) } == true }) else {
            throw CocoaError(.validationMultipleErrors, userInfo: [
                NSLocalizedDescriptionKey: "Random Stack produced an invalid effect combination."
            ])
        }
    }

    private static func weighted(_ values: [Int32], using random: inout SplitMix64) -> Int32 {
        values[random.integer(in: 0...(values.count - 1))]
    }

    private static func deterministicUUID(using random: inout SplitMix64) -> UUID {
        let high = random.next().bigEndian
        let low = random.next().bigEndian
        let bytes = withUnsafeBytes(of: high) { Array($0) } + withUnsafeBytes(of: low) { Array($0) }
        return UUID(uuid: (
            bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
            bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]
        ))
    }
}

private extension Float {
    static func triangular(
        _ range: ClosedRange<Float>,
        preferred: Float,
        using random: inout SplitMix64
    ) -> Float {
        RandomFloatDistribution.triangular(range, preferred: preferred).sample(using: &random)
    }
}
