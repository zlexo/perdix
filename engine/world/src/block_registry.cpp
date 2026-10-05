#include <vx/world/block_registry.hpp>

#include <vx/core/diagnostic.hpp>
#include <vx/world/block_file.hpp>
#include <vx/world/block_state.hpp>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vx {
namespace {

// A 16-bit state id numbers 65,536 states, air among them (RFC 0002).
constexpr std::uint64_t max_states = std::uint64_t{std::numeric_limits<std::uint16_t>::max()} + 1;

// 70000 as "70,000", for messages.
std::string with_commas(std::uint64_t number) {
    std::string digits = std::to_string(number);
    for (std::size_t at = digits.size(); at > 3; at -= 3) {
        digits.insert(at - 3, ",");
    }
    return digits;
}

// The product of the property value counts. It saturates instead of
// overflowing, so a block of many properties cannot wrap it, and short of
// that the error for too many states gives every count as it is.
std::uint64_t states_of(const BlockDefinition& block) {
    constexpr std::uint64_t most = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t states = 1;
    for (const PropertyDefinition& property : block.properties) {
        const std::uint64_t values = property.values.size();
        states = values != 0 && states > most / values ? most : states * values;
    }
    return states;
}

// The error for more states than ids, naming the largest blocks, as Bedrock
// names them when its permutations overflow.
Diagnostic too_many_states(std::uint64_t total, std::span<const BlockDefinition> blocks) {
    std::vector<const BlockDefinition*> largest;
    largest.reserve(blocks.size());
    for (const BlockDefinition& block : blocks) {
        largest.push_back(&block);
    }
    std::ranges::stable_sort(largest, std::ranges::greater{},
                             [](const BlockDefinition* block) { return states_of(*block); });
    largest.resize(std::min<std::size_t>(largest.size(), 3));
    std::string message = "the blocks have " + with_commas(total) + " states, more than the " +
                          with_commas(max_states) + " a state id numbers; the largest: ";
    for (std::size_t at = 0; at < largest.size(); ++at) {
        message += (at == 0 ? "" : ", ") + largest[at]->name + " (" +
                   with_commas(states_of(*largest[at])) + ")";
    }
    return {.severity = Severity::error,
            .file = largest.front()->path,
            .line = 0,
            .column = 0,
            .message = message};
}

} // namespace

BlockLoad BlockRegistry::load(std::span<const SourceFile> files) {
    // In path order, so the diagnostics come out the same whatever order the
    // files arrive in; the ids never depend on it.
    std::vector<const SourceFile*> ordered;
    ordered.reserve(files.size());
    for (const SourceFile& file : files) {
        ordered.push_back(&file);
    }
    std::ranges::sort(ordered, {}, &SourceFile::path);

    BlockLoad load;
    std::vector<BlockDefinition> definitions;
    for (const SourceFile* const file : ordered) {
        if (std::optional<BlockDefinition> definition = parse_block_file(*file, load.diagnostics)) {
            definitions.push_back(*std::move(definition));
        }
    }
    // Block names in byte order, which numbers the blocks.
    std::ranges::stable_sort(definitions, {}, &BlockDefinition::name);
    for (std::size_t at = 1; at < definitions.size(); ++at) {
        if (definitions[at].name == definitions[at - 1].name) {
            load.diagnostics.push_back({.severity = Severity::error,
                                        .file = definitions[at].path,
                                        .line = 0,
                                        .column = 0,
                                        .message = definitions[at].name +
                                                   " is defined twice, also in " +
                                                   definitions[at - 1].path});
        }
    }
    std::uint64_t total = 1; // air
    for (const BlockDefinition& definition : definitions) {
        const std::uint64_t states = states_of(definition);
        constexpr std::uint64_t most = std::numeric_limits<std::uint64_t>::max();
        total = states > most - total ? most : total + states;
    }
    if (total > max_states) {
        load.diagnostics.push_back(too_many_states(total, definitions));
    }
    if (has_errors(load.diagnostics)) {
        return load;
    }

    BlockRegistry registry;
    registry.blocks_.push_back({.name = "core:air", .first = 0, .states = 1, .properties = {}});
    std::uint32_t next = 1;
    for (BlockDefinition& definition : definitions) {
        Block block{.name = definition.name,
                    .first = next,
                    .states = static_cast<std::uint32_t>(states_of(definition)),
                    .properties = {}};
        for (PropertyDefinition& property : definition.properties) {
            block.properties.push_back(
                {.name = std::move(property.name), .values = std::move(property.values)});
        }
        next += block.states;
        registry.blocks_.push_back(std::move(block));
    }
    registry.types_.reserve(next);
    for (std::size_t type = 0; type < registry.blocks_.size(); ++type) {
        registry.types_.insert(registry.types_.end(), registry.blocks_[type].states,
                               static_cast<BlockType>(type));
    }
    load.registry = std::move(registry);
    return load;
}

std::optional<BlockState> BlockRegistry::find(std::string_view key) const {
    const std::size_t bracket = key.find('[');
    const Block* const block = find_block(key.substr(0, bracket));
    if (block == nullptr) {
        return std::nullopt;
    }
    std::vector<std::size_t> values(block->properties.size(), 0);
    if (bracket == std::string_view::npos) {
        return state_of(*block, values);
    }
    // "[facing=east,lit=true]": each property once, in any order.
    if (!key.ends_with(']') || key.size() - bracket < 3) {
        return std::nullopt;
    }
    std::vector<bool> given(block->properties.size(), false);
    std::string_view list = key.substr(bracket + 1, key.size() - bracket - 2);
    while (true) {
        const std::size_t comma = list.find(',');
        const std::string_view pair = list.substr(0, comma);
        const std::size_t equals = pair.find('=');
        if (equals == std::string_view::npos) {
            return std::nullopt;
        }
        const auto property =
            std::ranges::find(block->properties, pair.substr(0, equals), &Property::name);
        if (property == block->properties.end()) {
            return std::nullopt;
        }
        const auto value = std::ranges::find(property->values, pair.substr(equals + 1));
        const auto index = static_cast<std::size_t>(property - block->properties.begin());
        if (value == property->values.end() || given[index]) {
            return std::nullopt;
        }
        given[index] = true;
        values[index] = static_cast<std::size_t>(value - property->values.begin());
        if (comma == std::string_view::npos) {
            break;
        }
        list.remove_prefix(comma + 1);
    }
    return state_of(*block, values);
}

std::string BlockRegistry::key(BlockState state) const {
    const Block& block = blocks_[static_cast<std::size_t>(type_of(state))];
    std::string key = block.name;
    if (block.properties.empty()) {
        return key;
    }
    const std::vector<std::size_t> values = values_of(state);
    key += '[';
    for (std::size_t at = 0; at < block.properties.size(); ++at) {
        key += (at == 0 ? "" : ",") + block.properties[at].name + '=' +
               block.properties[at].values[values[at]];
    }
    key += ']';
    return key;
}

std::optional<BlockState> BlockRegistry::with(BlockState state, std::string_view property,
                                              std::string_view value) const {
    const Block& block = blocks_[static_cast<std::size_t>(type_of(state))];
    const auto changed = std::ranges::find(block.properties, property, &Property::name);
    if (changed == block.properties.end()) {
        return std::nullopt;
    }
    const auto index = std::ranges::find(changed->values, value);
    if (index == changed->values.end()) {
        return std::nullopt;
    }
    std::vector<std::size_t> values = values_of(state);
    values[static_cast<std::size_t>(changed - block.properties.begin())] =
        static_cast<std::size_t>(index - changed->values.begin());
    return state_of(block, values);
}

BlockType BlockRegistry::type_of(BlockState state) const noexcept {
    assert(static_cast<std::size_t>(state) < types_.size());
    return types_[static_cast<std::size_t>(state)];
}

BlockState BlockRegistry::default_state(BlockType type) const noexcept {
    return static_cast<BlockState>(blocks_[static_cast<std::size_t>(type)].first);
}

const BlockRegistry::Block* BlockRegistry::find_block(std::string_view name) const {
    if (name == blocks_.front().name) {
        return &blocks_.front();
    }
    // Every block but air is in name order.
    const auto rest = std::span(blocks_).subspan(1);
    const auto found = std::ranges::lower_bound(rest, name, {}, &Block::name);
    return found != rest.end() && found->name == name ? &*found : nullptr;
}

BlockState BlockRegistry::state_of(const Block& block, std::span<const std::size_t> values) {
    // Mixed radix, the last property varying fastest: "facing=east,lit=true"
    // is 1 * 2 + 1 past the furnace's first state.
    std::size_t offset = 0;
    for (std::size_t at = 0; at < block.properties.size(); ++at) {
        offset = (offset * block.properties[at].values.size()) + values[at];
    }
    return static_cast<BlockState>(block.first + offset);
}

std::vector<std::size_t> BlockRegistry::values_of(BlockState state) const {
    const Block& block = blocks_[static_cast<std::size_t>(type_of(state))];
    std::vector<std::size_t> values(block.properties.size());
    std::size_t offset = static_cast<std::size_t>(state) - block.first;
    for (std::size_t at = block.properties.size(); at-- > 0;) {
        const std::size_t count = block.properties[at].values.size();
        values[at] = offset % count;
        offset /= count;
    }
    return values;
}

} // namespace vx
