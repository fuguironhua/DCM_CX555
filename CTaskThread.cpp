#include "pch.h"
#include "CTaskThread.h"
#include "CWndManager.h"
#include <cmath>
#include <cstdarg>
#include <cstring>
#include <functional>
#include <fstream>
#include <filesystem>
#include <mutex>
#include <cwctype>
#include <regex>
#include <string>
#include <vector>
#include <algorithm>
#include <unordered_set>

namespace {

// Updated 32-bit Game.exe instruction: mov ecx, dword ptr ds:[00B46034]
// (Game.exe base 0x00380000 + RVA 0x007C6034).
// The value stored there is the ride-list object.  The first ride record is
// inline at object + 0x805AC; record[0] contains id at +0x00 and the usable
// / highlighted flag at +0x04.
constexpr LONGLONG kRideTableOffset = 0x805AC;

// CEGUI WindowManager 的注册表是 std::_Tree。WindowManager 保存树头，
// 树头 + 0x04 保存根节点；节点字段来自当前 32 位客户端布局。
constexpr DWORD kCeguiTreeHeadOffset = 0x00;
constexpr DWORD kCeguiTreeRootOffset = 0x04;
constexpr DWORD kCeguiTreeLeftOffset = 0x00;
constexpr DWORD kCeguiTreeRightOffset = 0x08;
constexpr DWORD kCeguiTreeNameOffset = 0x10;
constexpr DWORD kCeguiTreeNameLengthOffset = 0x20;
constexpr DWORD kCeguiTreeNameCapacityOffset = 0x24;
constexpr DWORD kCeguiTreeWindowOffset = 0x28;
constexpr DWORD kCeguiMaxNameLength = 0x400;
constexpr size_t kCeguiMaxVisitedNodes = 128;

// Active character states are held in a std::_Tree reached through the first
// global below.  The second global owns the sorted state-definition array.
// These are module RVAs, so they remain valid when Game.exe is rebased.
constexpr DWORD kStateBufferGlobalRva = 0x007C6034;
constexpr DWORD kStateBufferTreeOffset = 0x6740C;
constexpr DWORD kStateBufferCountOffset = 0x67410;
constexpr DWORD kStateBufferNodeLeftOffset = 0x00;
constexpr DWORD kStateBufferNodeParentOffset = 0x04;
constexpr DWORD kStateBufferNodeRightOffset = 0x08;
constexpr DWORD kStateBufferNodeValueOffset = 0x14;
constexpr DWORD kStateBufferEntryIdOffset = 0x08;

// Target/pet objects expose a different tree layout to the Lua target API.
// The object stores a tree-head pointer at +0x130 and the active node count
// at +0x134; each tree node stores the status ID directly at +0x10.
constexpr DWORD kTargetStateTreeOffset = 0x130;
constexpr DWORD kTargetStateCountOffset = 0x134;
constexpr DWORD kTargetStateNodeValueOffset = 0x10;

constexpr DWORD kStateDefinitionGlobalRva = 0x007D2704;
constexpr DWORD kStateDefinitionManagerOffset = 0x04;
constexpr DWORD kStateDefinitionCountOffset = 0x14;
constexpr DWORD kStateDefinitionArrayOffset = 0x30;
constexpr DWORD kStateDefinitionValidOffset = 0x40;
constexpr DWORD kStateObjectIdOffset = 0x00;
constexpr DWORD kStateObjectNameOffset = 0x0C;
constexpr DWORD kStateObjectDescriptionOffset = 0x40;

// The target resolver loads dword_1325AF0 before resolving the selected object.
// It is a module-global pointer slot (Game.exe + 0x7C5AF0); its selected
// CObject_PlayerOther cache is stored at object-manager + 0x48.
constexpr DWORD kObjectManagerGlobalRva = 0x007C5AF0;
constexpr DWORD kObjectManagerTargetOffset = 0x48;
constexpr DWORD kObjectManagerTreeOffset = 0x50;
constexpr DWORD kObjectManagerNodeKeyOffset = 0x10;
constexpr DWORD kObjectManagerNodeValueOffset = 0x14;
constexpr DWORD kSelectionHandleOffset = 0x22BF8;

// Pet table: seven records of PET_RANG bytes.  The out-state fields live
// on the character object and are compared with the pet record fields.
constexpr DWORD kPetRecordSize = 524;          // PET_RANG
constexpr DWORD kPetTableOffset = 0x6711C;
constexpr DWORD kPetSkillArrayOffset = 0x11C;
constexpr DWORD kPetSkillSlotCount = 13;
// SkillTemplate_V1: 696=血祭, 697=高级血祭.
constexpr DWORD kPetXueJiSkillId = 696;
constexpr DWORD kPetGaoJiXueJiSkillId = 697;
constexpr DWORD kPetOutStatusOffset = 0x2A00;   // cwcz1
constexpr DWORD kPetSoulOffset = kPetOutStatusOffset - 380;

constexpr DWORD kMinimumUserAddress = 0x10000;
constexpr DWORD kMaximumStateEntries = 0x10000;
constexpr size_t kMaximumStateTreeSteps = 0x10000;

bool IsPlausibleStatePointer(DWORD address) {
    return address >= kMinimumUserAddress;
}

DWORD ReadStateBufferFromObject(dmsoft* dm, long targetHwnd, DWORD gameModule,
                                DWORD characterObject, DWORD xuh,
                                CString* name, CString* description,
                                DWORD* stateObject) {
    if (name != nullptr) name->Empty();
    if (description != nullptr) description->Empty();
    if (stateObject != nullptr) *stateObject = 0;

    if (dm == nullptr || gameModule == 0 ||
        !IsPlausibleStatePointer(characterObject)) {
        return 0;
    }

    const auto readDword = [dm, targetHwnd](DWORD address) -> DWORD {
        return static_cast<DWORD>(dm->ReadIntAddr(
            targetHwnd, static_cast<LONGLONG>(address), 4));
    };

    // C7C7D0 walks the active-state std::_Tree in in-order order.  Its
    // sentinel stores the leftmost node at +0 and the root at +4; each node
    // stores left/parent/right at +0/+4/+8 and the value entry at +0x14.
    const DWORD stateCount = readDword(
        characterObject + kStateBufferCountOffset);
    if (stateCount == 0 || stateCount > kMaximumStateEntries ||
        xuh >= stateCount) {
        return 0;
    }

    const DWORD treeHead = readDword(
        characterObject + kStateBufferTreeOffset);
    if (!IsPlausibleStatePointer(treeHead)) return 0;

    DWORD node = readDword(treeHead + kStateBufferNodeLeftOffset);
    if (!IsPlausibleStatePointer(node) || node == treeHead) return 0;

    std::unordered_set<DWORD> visited;
    visited.reserve(static_cast<size_t>((std::min)(
        stateCount, static_cast<DWORD>(256))));
    DWORD entry = 0;

    for (DWORD ordinal = 0; ordinal <= xuh; ++ordinal) {
        if (!IsPlausibleStatePointer(node) || node == treeHead ||
            !visited.insert(node).second ||
            visited.size() > kMaximumStateTreeSteps) {
            return 0;
        }

        // The byte at +0x0D is the std::_Tree sentinel marker (isnil).
        if ((readDword(node + 0x0D) & 0xFFu) != 0) return 0;

        if (ordinal == xuh) {
            entry = readDword(node + kStateBufferNodeValueOffset);
            break;
        }

        const DWORD right = readDword(node + kStateBufferNodeRightOffset);
        if (right != 0 && right != treeHead) {
            DWORD next = right;
            size_t depth = 0;
            for (; depth < kMaximumStateTreeSteps; ++depth) {
                if (!IsPlausibleStatePointer(next) || next == treeHead) {
                    return 0;
                }
                const DWORD left = readDword(
                    next + kStateBufferNodeLeftOffset);
                if (left == 0 || left == treeHead) break;
                next = left;
            }
            if (depth == kMaximumStateTreeSteps) return 0;
            node = next;
        } else {
            DWORD current = node;
            DWORD parent = readDword(
                current + kStateBufferNodeParentOffset);
            size_t ascent = 0;
            for (; parent != 0 && parent != treeHead &&
                   ascent < kMaximumStateTreeSteps; ++ascent) {
                if (!IsPlausibleStatePointer(parent)) return 0;
                if (current != readDword(
                        parent + kStateBufferNodeRightOffset)) {
                    break;
                }
                current = parent;
                parent = readDword(
                    parent + kStateBufferNodeParentOffset);
            }
            if (ascent == kMaximumStateTreeSteps) return 0;
            node = parent;
        }
    }

    if (!IsPlausibleStatePointer(entry)) return 0;

    // [entry+8] is read by the original movsx instruction, therefore the
    // comparison key is a signed 16-bit value rather than a full DWORD.
    const SHORT stateId = static_cast<SHORT>(static_cast<WORD>(
        readDword(entry + kStateBufferEntryIdOffset)));

    // 108BB10 is only a virtual thunk.  Its implementation (F10DB0)
    // performs a signed binary search over manager+0x30, using manager+0x14
    // as count and manager+0x40 as the non-empty guard.  Reproduce that data
    // path directly so no target code is called or patched.
    const DWORD definitionWrapper = readDword(
        gameModule + kStateDefinitionGlobalRva);
    if (!IsPlausibleStatePointer(definitionWrapper)) return 0;
    const DWORD definitionManager = readDword(
        definitionWrapper + kStateDefinitionManagerOffset);
    if (!IsPlausibleStatePointer(definitionManager)) return 0;

    const DWORD definitionCount = readDword(
        definitionManager + kStateDefinitionCountOffset);
    const DWORD definitionValid = readDword(
        definitionManager + kStateDefinitionValidOffset);
    const DWORD definitionArray = readDword(
        definitionManager + kStateDefinitionArrayOffset);
    if (definitionCount == 0 || definitionCount > kMaximumStateEntries ||
        definitionValid == 0 || !IsPlausibleStatePointer(definitionArray)) {
        return 0;
    }

    LONG low = 0;
    LONG high = static_cast<LONG>(definitionCount - 1);
    DWORD object = 0;
    while (low <= high) {
        const LONG middle = low + (high - low) / 2;
        const DWORD candidate = readDword(
            definitionArray + static_cast<DWORD>(middle) * sizeof(DWORD));
        if (!IsPlausibleStatePointer(candidate)) return 0;

        const LONG candidateId = static_cast<LONG>(
            readDword(candidate + kStateObjectIdOffset));
        if (candidateId < static_cast<LONG>(stateId)) {
            low = middle + 1;
        } else if (candidateId > static_cast<LONG>(stateId)) {
            high = middle - 1;
        } else {
            object = candidate;
            break;
        }
    }
    if (!IsPlausibleStatePointer(object)) return 0;

    const DWORD objectId = readDword(object + kStateObjectIdOffset);
    if (static_cast<LONG>(objectId) != static_cast<LONG>(stateId)) return 0;

    if (stateObject != nullptr) *stateObject = object;
    if (name != nullptr) {
        const DWORD nameAddress = readDword(
            object + kStateObjectNameOffset);
        if (IsPlausibleStatePointer(nameAddress)) {
            *name = dm->ReadStringAddr(
                targetHwnd, static_cast<LONGLONG>(nameAddress), 0, 0);
        }
    }
    if (description != nullptr) {
        const DWORD descriptionAddress = readDword(
            object + kStateObjectDescriptionOffset);
        if (IsPlausibleStatePointer(descriptionAddress)) {
            *description = dm->ReadStringAddr(
                targetHwnd, static_cast<LONGLONG>(descriptionAddress), 0, 0);
        }
    }

    return objectId;
}

DWORD ReadTargetStateBufferFromObject(
    dmsoft* dm, long targetHwnd, DWORD gameModule, DWORD targetObject,
    DWORD xuh, CString* name, CString* description, DWORD* stateObject) {
    if (name != nullptr) name->Empty();
    if (description != nullptr) description->Empty();
    if (stateObject != nullptr) *stateObject = 0;

    if (dm == nullptr || gameModule == 0 ||
        !IsPlausibleStatePointer(targetObject)) {
        return 0;
    }

    const auto readDword = [dm, targetHwnd](DWORD address) -> DWORD {
        return static_cast<DWORD>(dm->ReadIntAddr(
            targetHwnd, static_cast<LONGLONG>(address), 4));
    };

    const DWORD stateCount = readDword(
        targetObject + kTargetStateCountOffset);
    if (stateCount == 0 || stateCount > kMaximumStateEntries ||
        xuh >= stateCount) {
        return 0;
    }

    const DWORD treeHead = readDword(
        targetObject + kTargetStateTreeOffset);
    if (!IsPlausibleStatePointer(treeHead)) return 0;

    // The target API starts at the leftmost node and advances in-order.
    DWORD node = readDword(treeHead + kStateBufferNodeLeftOffset);
    if (!IsPlausibleStatePointer(node) || node == treeHead) return 0;

    std::unordered_set<DWORD> visited;
    visited.reserve(static_cast<size_t>((std::min)(
        stateCount, static_cast<DWORD>(256))));
    DWORD stateId = 0;

    for (DWORD ordinal = 0; ordinal <= xuh; ++ordinal) {
        if (!IsPlausibleStatePointer(node) || node == treeHead ||
            !visited.insert(node).second ||
            visited.size() > kMaximumStateTreeSteps) {
            return 0;
        }

        // The byte at +0x0D is the std::_Tree sentinel marker (isnil).
        if ((readDword(node + 0x0D) & 0xFFu) != 0) return 0;

        if (ordinal == xuh) {
            stateId = readDword(node + kTargetStateNodeValueOffset);
            break;
        }

        const DWORD right = readDword(node + kStateBufferNodeRightOffset);
        if (right != 0 && right != treeHead) {
            DWORD next = right;
            size_t depth = 0;
            for (; depth < kMaximumStateTreeSteps; ++depth) {
                if (!IsPlausibleStatePointer(next) || next == treeHead) {
                    return 0;
                }
                const DWORD left = readDword(
                    next + kStateBufferNodeLeftOffset);
                if (left == 0 || left == treeHead) break;
                next = left;
            }
            if (depth == kMaximumStateTreeSteps) return 0;
            node = next;
        } else {
            DWORD current = node;
            DWORD parent = readDword(
                current + kStateBufferNodeParentOffset);
            size_t ascent = 0;
            for (; parent != 0 && parent != treeHead &&
                   ascent < kMaximumStateTreeSteps; ++ascent) {
                if (!IsPlausibleStatePointer(parent)) return 0;
                if (current != readDword(
                        parent + kStateBufferNodeRightOffset)) {
                    break;
                }
                current = parent;
                parent = readDword(
                    parent + kStateBufferNodeParentOffset);
            }
            if (ascent == kMaximumStateTreeSteps) return 0;
            node = parent;
        }
    }

    if (stateId == 0) return 0;

    // Reuse the same sorted definition array as the native GetAllBuff path.
    const DWORD definitionWrapper = readDword(
        gameModule + kStateDefinitionGlobalRva);
    if (!IsPlausibleStatePointer(definitionWrapper)) return 0;
    const DWORD definitionManager = readDword(
        definitionWrapper + kStateDefinitionManagerOffset);
    if (!IsPlausibleStatePointer(definitionManager)) return 0;

    const DWORD definitionCount = readDword(
        definitionManager + kStateDefinitionCountOffset);
    const DWORD definitionValid = readDword(
        definitionManager + kStateDefinitionValidOffset);
    const DWORD definitionArray = readDword(
        definitionManager + kStateDefinitionArrayOffset);
    if (definitionCount == 0 || definitionCount > kMaximumStateEntries ||
        definitionValid == 0 || !IsPlausibleStatePointer(definitionArray)) {
        return 0;
    }

    LONG low = 0;
    LONG high = static_cast<LONG>(definitionCount - 1);
    DWORD object = 0;
    while (low <= high) {
        const LONG middle = low + (high - low) / 2;
        const DWORD candidate = readDword(
            definitionArray + static_cast<DWORD>(middle) * sizeof(DWORD));
        if (!IsPlausibleStatePointer(candidate)) return 0;

        const LONG candidateId = static_cast<LONG>(
            readDword(candidate + kStateObjectIdOffset));
        if (candidateId < static_cast<LONG>(stateId)) {
            low = middle + 1;
        } else if (candidateId > static_cast<LONG>(stateId)) {
            high = middle - 1;
        } else {
            object = candidate;
            break;
        }
    }
    if (!IsPlausibleStatePointer(object)) return 0;

    const DWORD objectId = readDword(object + kStateObjectIdOffset);
    if (objectId != stateId) return 0;

    if (stateObject != nullptr) *stateObject = object;
    if (name != nullptr) {
        const DWORD nameAddress = readDword(
            object + kStateObjectNameOffset);
        if (IsPlausibleStatePointer(nameAddress)) {
            *name = dm->ReadStringAddr(
                targetHwnd, static_cast<LONGLONG>(nameAddress), 0, 0);
        }
    }
    if (description != nullptr) {
        const DWORD descriptionAddress = readDword(
            object + kStateObjectDescriptionOffset);
        if (IsPlausibleStatePointer(descriptionAddress)) {
            *description = dm->ReadStringAddr(
                targetHwnd, static_cast<LONGLONG>(descriptionAddress), 0, 0);
        }
    }

    return objectId;
}

// ObjectManager's +0x50 is the ordered object tree used by its vtable+0x44
// lookup. Reproduce that lookup in memory so a selected handle can be
// resolved even when the resolver's +0x48 cache has not been refreshed yet.
DWORD FindObjectByHandle(dmsoft* dm, long targetHwnd, DWORD objectManager,
                         LONG handle) {
    if (dm == nullptr || !IsPlausibleStatePointer(objectManager) ||
        handle < 0) {
        return 0;
    }

    const auto readDword = [dm, targetHwnd](DWORD address) -> DWORD {
        return static_cast<DWORD>(dm->ReadIntAddr(
            targetHwnd, static_cast<LONGLONG>(address), 4));
    };

    const DWORD treeHead = readDword(
        objectManager + kObjectManagerTreeOffset);
    if (!IsPlausibleStatePointer(treeHead)) return 0;

    DWORD node = readDword(treeHead + 0x04);
    DWORD candidate = treeHead;
    size_t steps = 0;
    while (IsPlausibleStatePointer(node) && node != treeHead &&
           steps++ < kMaximumStateTreeSteps) {
        // MSVC std::_Tree stores the sentinel marker at the high byte of the
        // node's +0x0C field; read from +0x0D as the game does.
        if ((readDword(node + 0x0D) & 0xFFu) != 0) break;

        const LONG nodeKey = static_cast<LONG>(
            readDword(node + kObjectManagerNodeKeyOffset));
        if (handle <= nodeKey) {
            candidate = node;
            node = readDword(node + 0x00);
        } else {
            node = readDword(node + 0x08);
        }
    }

    if (candidate == treeHead || !IsPlausibleStatePointer(candidate) ||
        static_cast<LONG>(readDword(
            candidate + kObjectManagerNodeKeyOffset)) != handle) {
        return 0;
    }

    const DWORD object = readDword(
        candidate + kObjectManagerNodeValueOffset);
    return IsPlausibleStatePointer(object) ? object : 0;
}

// Walk an active-status std::_Tree in sorted order. The status trees used by
// the player and target objects share the same node layout: left/parent/right
// at +0/+4/+8, the sentinel marker at +0x0D, and the status ID at +0x10.
// This iterative, cycle-checked traversal avoids the stale-parent and
// sentinel recursion problems in the old NerbArtest implementation.
bool CollectStatusTreeValues(dmsoft* dm, long targetHwnd, DWORD treeHead,
                             DWORD count, std::vector<DWORD>& values) {
    if (dm == nullptr || !IsPlausibleStatePointer(treeHead) ||
        count == 0 || count > kMaximumStateEntries) {
        return false;
    }

    const auto readDword = [dm, targetHwnd](DWORD address) -> DWORD {
        return static_cast<DWORD>(dm->ReadIntAddr(
            targetHwnd, static_cast<LONGLONG>(address), 4));
    };

    const DWORD root = readDword(treeHead + kStateBufferNodeParentOffset);
    if (!IsPlausibleStatePointer(root) || root == treeHead) return false;

    std::vector<DWORD> pending;
    pending.reserve(static_cast<size_t>((std::min)(count,
                                                    static_cast<DWORD>(256))));
    std::unordered_set<DWORD> visited;
    visited.reserve(static_cast<size_t>((std::min)(count,
                                                    static_cast<DWORD>(256))));

    DWORD node = root;
    while ((!pending.empty() ||
            (IsPlausibleStatePointer(node) && node != treeHead)) &&
           values.size() < count) {
        while (IsPlausibleStatePointer(node) && node != treeHead) {
            if (!visited.insert(node).second ||
                visited.size() > kMaximumStateTreeSteps) {
                return false;
            }
            pending.push_back(node);
            node = readDword(node + kStateBufferNodeLeftOffset);
        }

        if (pending.empty()) break;
        node = pending.back();
        pending.pop_back();

        // +0x0D is std::_Tree's isnil byte. A marked node is the sentinel,
        // not an active status entry.
        if ((readDword(node + 0x0D) & 0xFFu) != 0) return false;
        values.push_back(readDword(node + kTargetStateNodeValueOffset));
        node = readDword(node + kStateBufferNodeRightOffset);
    }

    return !values.empty();
}

// 跑商任务记录布局（DataPool + 0x28 指向的记录表）。
constexpr DWORD kPaoShangMissionId = 2115;
constexpr DWORD kMissionTableOffset = 0x28;
constexpr DWORD kMissionRecordStride = 0x29;
constexpr DWORD kMissionIdOffset = 0x09;
constexpr DWORD kPaoShangZijinOffset = 0x0E;
constexpr int kMissionRecordCount = 20;

// Guild-rank data used by the client-side title switch at Game.exe+DCA002:
// [Game.exe+0x7C6034] yields the player-data object, whose +0x61B90 byte is
// the raw rank value (2..9: member, elite, merchant, ... guild master).
constexpr DWORD kGuildPlayerGlobalRva = 0x007C7034;
constexpr DWORD kGuildRankOffset = 0x61B90;

struct KjdaEntry {
    std::wstring question;
    std::wstring answer;
};

std::wstring KjdaUtf8ToWide(const std::string& value) {
    if (value.empty()) return {};

    int length = ::MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) {
        length = ::MultiByteToWideChar(
            CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
            nullptr, 0);
    }
    if (length <= 0) return {};

    std::wstring result(static_cast<size_t>(length), L'\0');
    ::MultiByteToWideChar(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        result.data(), length);
    return result;
}

std::vector<std::string> KjdaParseCsvLine(const std::string& line) {
    std::vector<std::string> fields;
    std::string field;
    bool quoted = false;

    for (size_t index = 0; index < line.size(); ++index) {
        const char character = line[index];
        if (character == '"') {
            if (quoted && index + 1 < line.size() && line[index + 1] == '"') {
                field.push_back('"');
                ++index;
            } else {
                quoted = !quoted;
            }
        } else if (character == ',' && !quoted) {
            fields.push_back(std::move(field));
            field.clear();
        } else {
            field.push_back(character);
        }
    }
    fields.push_back(std::move(field));
    return fields;
}

bool KjdaPunctuation(wchar_t character) {
    switch (static_cast<unsigned int>(character)) {
    case 0x3001: // ideographic comma
    case 0x3002: // ideographic full stop
    case 0x3008: case 0x3009: // angle brackets
    case 0x300A: case 0x300B: // double angle brackets
    case 0x300C: case 0x300D: // corner brackets
    case 0x300E: case 0x300F: // white corner brackets
    case 0x3010: case 0x3011: // lenticular brackets
    case 0x3014: case 0x3015:
    case 0x2018: case 0x2019: // single quotes
    case 0x201C: case 0x201D: // double quotes
    case 0x2026: // ellipsis
    case 0x2014: // em dash
    case 0x00B7: // middle dot
    case 0xFF01: // fullwidth exclamation mark
    case 0xFF08: case 0xFF09: // fullwidth parentheses
    case 0xFF0C: // fullwidth comma
    case 0xFF1A: // fullwidth colon
    case 0xFF1B: // fullwidth semicolon
    case 0xFF1F: // fullwidth question mark
    case 0xFF5E: // fullwidth tilde
        return true;
    default:
        break;
    }

    return (character >= L'!' && character <= L'/') ||
           (character >= L':' && character <= L'@') ||
           (character >= L'[' && character <= L'`') ||
           (character >= L'{' && character <= L'~');
}

std::wstring KjdaNormalize(const std::wstring& text) {
    std::wstring result;
    result.reserve(text.size());
    for (wchar_t character : text) {
        if (std::iswspace(character) || character == 0x3000 ||
            KjdaPunctuation(character)) continue;
        result.push_back(static_cast<wchar_t>(std::towlower(character)));
    }
    return result;
}

double KjdaLevenshteinSimilarity(const std::wstring& left,
                                 const std::wstring& right) {
    if (left.empty() || right.empty()) {
        return left.empty() && right.empty() ? 1.0 : 0.0;
    }
    if (left == right) return 1.0;

    const std::wstring* shorter = &left;
    const std::wstring* longer = &right;
    if (shorter->size() > longer->size()) std::swap(shorter, longer);

    std::vector<size_t> previous(shorter->size() + 1);
    std::vector<size_t> current(shorter->size() + 1);
    for (size_t index = 0; index <= shorter->size(); ++index) {
        previous[index] = index;
    }

    for (size_t row = 1; row <= longer->size(); ++row) {
        current[0] = row;
        for (size_t column = 1; column <= shorter->size(); ++column) {
            const size_t substitution = previous[column - 1] +
                ((*longer)[row - 1] == (*shorter)[column - 1] ? 0 : 1);
            current[column] = (std::min)({
                current[column - 1] + 1,
                previous[column] + 1,
                substitution
            });
        }
        std::swap(previous, current);
    }

    return 1.0 - static_cast<double>(previous[shorter->size()]) /
        static_cast<double>(longer->size());
}

double KjdaSimilarity(const std::wstring& query, const std::wstring& answer) {
    if (query.empty() || answer.empty()) return 0.0;
    if (query == answer) return 1.0;

    // A question fragment may come from the middle of the original question.
    // Treat an exact fragment of at least four characters as a full match.
    if (query.size() >= 4 && answer.find(query) != std::wstring::npos) {
        return 1.0;
    }

    double best = KjdaLevenshteinSimilarity(query, answer);

    // Longest common substring handles a contiguous fragment without creating
    // and comparing every possible answer window.
    std::vector<size_t> previous(query.size() + 1, 0);
    std::vector<size_t> current(query.size() + 1, 0);
    size_t longestSubstring = 0;
    for (wchar_t answerCharacter : answer) {
        std::fill(current.begin(), current.end(), 0);
        for (size_t column = 1; column <= query.size(); ++column) {
            if (answerCharacter == query[column - 1]) {
                current[column] = previous[column - 1] + 1;
                longestSubstring = (std::max)(longestSubstring, current[column]);
            }
        }
        std::swap(previous, current);
    }
    best = (std::max)(best, static_cast<double>(longestSubstring) /
        static_cast<double>(query.size()));
    if (best >= 0.80) return best;

    // LCS tolerates one or two missing OCR characters inside a fragment while
    // still requiring the characters to remain in the original order.
    std::fill(previous.begin(), previous.end(), 0);
    for (wchar_t answerCharacter : answer) {
        current[0] = 0;
        for (size_t column = 1; column <= query.size(); ++column) {
            current[column] = answerCharacter == query[column - 1]
                ? previous[column - 1] + 1
                : (std::max)(previous[column], current[column - 1]);
        }
        std::swap(previous, current);
    }
    best = (std::max)(best, static_cast<double>(previous[query.size()]) /
        static_cast<double>(query.size()));
    return best;
}

CString KjdaCsvPath() {
    std::vector<CString> candidates;
    wchar_t module[MAX_PATH] = {};
    const DWORD moduleLength = ::GetModuleFileNameW(
        nullptr, module, _countof(module));
    if (moduleLength > 0 && moduleLength < _countof(module)) {
        std::filesystem::path modulePath(module);
        candidates.emplace_back((modulePath.parent_path() /
                                 L"17173_qna_unique.csv").c_str());
        candidates.emplace_back((modulePath.parent_path().parent_path() /
                                 L"Debug" / L"17173_qna_unique.csv").c_str());
    }

    wchar_t current[MAX_PATH] = {};
    const DWORD currentLength = ::GetCurrentDirectoryW(
        _countof(current), current);
    if (currentLength > 0 && currentLength < _countof(current)) {
        const std::filesystem::path currentPath(current);
        candidates.emplace_back((currentPath / L"17173_qna_unique.csv").c_str());
        candidates.emplace_back((currentPath / L"Debug" /
                                 L"17173_qna_unique.csv").c_str());
    }

    for (const CString& candidate : candidates) {
        if (::GetFileAttributesW(candidate.GetString()) !=
            INVALID_FILE_ATTRIBUTES) {
            return candidate;
        }
    }
    return candidates.empty() ? CString() : candidates.front();
}

const std::vector<KjdaEntry>& KjdaEntries() {
    static std::once_flag loadOnce;
    static std::vector<KjdaEntry> entries;
    std::call_once(loadOnce, [] {
        const CString path = KjdaCsvPath();
        if (path.IsEmpty()) return;

        std::ifstream input(std::filesystem::path(path.GetString()),
                            std::ios::binary);
        if (!input) return;

        std::string line;
        bool firstLine = true;
        while (std::getline(input, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const std::vector<std::string> fields = KjdaParseCsvLine(line);
            if (fields.size() < 2) continue;

            std::string question = fields[0];
            if (firstLine && question.size() >= 3 &&
                static_cast<unsigned char>(question[0]) == 0xEF &&
                static_cast<unsigned char>(question[1]) == 0xBB &&
                static_cast<unsigned char>(question[2]) == 0xBF) {
                question.erase(0, 3);
            }
            firstLine = false;
            if (question == "question") continue;

            KjdaEntry entry;
            entry.question = KjdaNormalize(KjdaUtf8ToWide(question));
            entry.answer = KjdaUtf8ToWide(fields[1]);
            if (!entry.question.empty()) entries.push_back(std::move(entry));
        }
    });
    return entries;
}

} // namespace

CTaskThread::CTaskThread(std::shared_ptr<tagWndInfo> wnd,
                         std::shared_ptr<CThreadControl> control,
                         const tagWndIni& ini,
                         HWND logWindow,
                         TaskMemoryStatePtr memory)
    : m_memory(memory ? std::move(memory) : std::make_shared<TaskMemoryState>()),
      jz(m_memory->addresses),
      tsg(m_memory->expressions),
      ck(m_memory->windowAddresses),
      Mams(m_memory->mams),
      bag(m_memory->bag),
      m_wnd(std::move(wnd)),
      m_control(std::move(control)),
      m_ini(ini),
      m_logWindow(logWindow) {}

CTaskThread::~CTaskThread() { Stop(); }

bool CTaskThread::Start() {
    if (m_running.exchange(true)) return false;
    m_control->Resume();
    m_thread = std::thread(&CTaskThread::ThreadMain, this);
    return true;
}

void CTaskThread::Stop() {
    m_control->Stop();
    if (m_thread.joinable()) m_thread.join();
    m_running.store(false);
    m_ready.store(false);
}

void CTaskThread::Suspend() { m_control->Pause(); }
void CTaskThread::Resume() { m_control->Resume(); }
bool CTaskThread::WaitIfPaused() { return m_control->WaitIfPaused(); }
bool CTaskThread::WaitFor(DWORD milliseconds) { return m_control->WaitFor(std::chrono::milliseconds(milliseconds)); }
bool CTaskThread::WaitForMemoryReady() {
    return m_memory && m_memory->WaitUntilInitialized([this] { return m_control->IsStopping(); });
}

bool CTaskThread::OnEventThreadRun() {
    if (!WaitIfPaused()) return false;
    if (!OnLoop()) return false;

    // Match the original CWHThread/CTaskThread cadence while keeping the
    // sleep interruptible by the cooperative stop/pause controller.
    SleepEx(1);
    return !m_control->IsStopping();
}

void CTaskThread::DebugLog(LPCTSTR format, ...) {
    va_list args;
    va_start(args, format);
    EmitFormattedDebugLog(format, args);
    va_end(args);
}

void CTaskThread::DebugLog(const CString& message) {
    EmitDebugLog(message);
}

void CTaskThread::LogD(LPCTSTR format, ...) {
    va_list args;
    va_start(args, format);
    EmitFormattedDebugLog(format, args);
    va_end(args);
}

void CTaskThread::EmitFormattedDebugLog(LPCTSTR format, va_list args) {
    CString text;
    if (format != nullptr) {
        text.FormatV(format, args);
    }
    EmitDebugLog(text);
}

void CTaskThread::EmitDebugLog(const CString& text) {
    // Keep the debug pane compact.  The dialog already adds the timestamp;
    // the window/thread metadata was useful while wiring the workers but made
    // every line needlessly wide for normal task output.
    CString output = text;
    output += _T("\r\n");
    OutputDebugString(output);

    if (m_logWindow && ::IsWindow(m_logWindow)) {
        auto* pending = new CString(text);
        if (!::PostMessage(m_logWindow, WM_APP_WINDOW_THREAD_LOG, 0, reinterpret_cast<LPARAM>(pending))) {
            delete pending;
        }
    }
}

DWORD CTaskThread::Read_Dw(CString cstr) {
    if (!m_dm || !m_ready.load() || !m_wnd || !m_wnd->hWnd) return 0;

    const long targetHwnd = static_cast<long>(reinterpret_cast<INT_PTR>(m_wnd->hWnd));
    return static_cast<DWORD>(m_dm->ReadInt(targetHwnd, cstr.GetString(), 4));
}

DWORD CTaskThread::Read_Dw(DWORD addr) {
    if (!m_dm || !m_ready.load() || !m_wnd || !m_wnd->hWnd) return 0;

    const long targetHwnd = static_cast<long>(reinterpret_cast<INT_PTR>(m_wnd->hWnd));
    return static_cast<DWORD>(m_dm->ReadIntAddr(targetHwnd, static_cast<LONGLONG>(addr), 4));
}

DWORD CTaskThread::FindCeguiWindowByName(const CString& name) {
    if (name.IsEmpty() || !m_dm || !m_ready.load() || !m_wnd || !m_wnd->hWnd) {
        return 0;
    }

    // ck[Manager] 是 CEGUIBase.dll + MANAGEP 的全局指针槽位，
    // 槽位内容才是 WindowManager*。
    const DWORD managerSlot = static_cast<DWORD>(ck[Manager]);
    if (managerSlot == 0) return 0;

    const DWORD manager = Read_Dw(managerSlot);
    if (manager == 0) return 0;

    const DWORD treeHead = Read_Dw(manager + kCeguiTreeHeadOffset);
    if (treeHead == 0) return 0;

    const DWORD root = Read_Dw(treeHead + kCeguiTreeRootOffset);
    if (root == 0 || root == treeHead) return 0;

    const long targetHwnd = static_cast<long>(reinterpret_cast<INT_PTR>(m_wnd->hWnd));
    // The registry is an ordered std::_Tree. Follow key ordering instead of
    // walking the entire tree on every price read.
    DWORD node = root;
    for (size_t step = 0; node != 0 && node != treeHead &&
                           step < kCeguiMaxVisitedNodes; ++step) {
        if (node < 0x10000) return 0;
        const DWORD length = Read_Dw(node + kCeguiTreeNameLengthOffset);
        const DWORD capacity = Read_Dw(node + kCeguiTreeNameCapacityOffset);
        if (length > 0 && length <= kCeguiMaxNameLength) {
            const DWORD stringStorage = Read_Dw(node + kCeguiTreeNameOffset);
            // MSVC std::string 在 capacity <= 0x0F 时使用节点内联缓冲区；
            // 否则 +0x10 保存堆缓冲区指针。
            const DWORD stringAddress = capacity <= 0x0F
                ? node + kCeguiTreeNameOffset
                : stringStorage;
            if (stringAddress != 0) {
                const CString nodeName = Dm()->ReadStringAddr(
                    targetHwnd, static_cast<LONGLONG>(stringAddress), 0,
                    static_cast<long>(length));
                const int order = nodeName.Compare(name);
                if (order == 0) {
                    return Read_Dw(node + kCeguiTreeWindowOffset);
                }
                node = order < 0
                    ? Read_Dw(node + kCeguiTreeRightOffset)
                    : Read_Dw(node + kCeguiTreeLeftOffset);
                continue;
            }
        }

        return 0;
    }

    return 0;
}

DWORD CTaskThread::ReadCeguiNumericText(DWORD window)
{
    if (window == 0 || !m_dm || !m_ready.load() || !m_wnd || !m_wnd->hWnd) {
        return 0;
    }

    const long targetHwnd = static_cast<long>(reinterpret_cast<INT_PTR>(m_wnd->hWnd));
    // CEGUI stores the rendered text as a token array: +0x60 is the token
    // count and +0x64 points to DWORD tokens. Digits are literal 0x30..0x39;
    // the remaining tokens are colour/font control markers.
    const DWORD tokenCount = static_cast<DWORD>(m_dm->ReadIntAddr(
        targetHwnd, static_cast<LONGLONG>(window) + 0x60, 4));
    const DWORD tokenAddress = static_cast<DWORD>(m_dm->ReadIntAddr(
        targetHwnd, static_cast<LONGLONG>(window) + 0x64, 4));
    if (tokenAddress == 0 || tokenCount == 0 || tokenCount > 64) return 0;

    unsigned long long value = 0;
    int digitCount = 0;
    for (DWORD index = 0; index < tokenCount; ++index) {
        const DWORD token = static_cast<DWORD>(m_dm->ReadIntAddr(
            targetHwnd, static_cast<LONGLONG>(tokenAddress) +
                static_cast<LONGLONG>(index) * 4, 4));
        if (token < static_cast<DWORD>(_T('0')) ||
            token > static_cast<DWORD>(_T('9'))) {
            continue;
        }
        value = value * 10ULL + (token - static_cast<DWORD>(_T('0')));
        if (value > 0xFFFFFFFFULL) return 0;
        ++digitCount;
    }

    return digitCount == 0 ? 0 : static_cast<DWORD>(value);
}

CString CTaskThread::ReadCeguiText(DWORD window)
{
    if (window == 0 || !m_dm || !m_ready.load() || !m_wnd || !m_wnd->hWnd) {
        return CString();
    }

    const long targetHwnd = static_cast<long>(reinterpret_cast<INT_PTR>(m_wnd->hWnd));
    // TLBB text widgets use the same token representation as the price
    // labels: +0x60 is the token count and +0x64 points to DWORD tokens.
    const DWORD tokenCount = static_cast<DWORD>(m_dm->ReadIntAddr(
        targetHwnd, static_cast<LONGLONG>(window) + 0x60, 4));
    const DWORD tokenAddress = static_cast<DWORD>(m_dm->ReadIntAddr(
        targetHwnd, static_cast<LONGLONG>(window) + 0x64, 4));
    if (tokenAddress == 0 || tokenCount == 0 || tokenCount > 1024) {
        return CString();
    }

    CString text;
    for (DWORD index = 0; index < tokenCount; ++index) {
        const DWORD token = static_cast<DWORD>(m_dm->ReadIntAddr(
            targetHwnd, static_cast<LONGLONG>(tokenAddress) +
                static_cast<LONGLONG>(index) * 4, 4));
        if (token == 0x09 || token == 0x0A || token == 0x0D) {
            text.AppendChar(static_cast<TCHAR>(token));
        } else if (token >= 0x20 && token <= 0xFFFF) {
            // Chinese characters and ASCII text are UTF-16 code units.  The
            // larger values are colour/font control tokens and are skipped.
            text.AppendChar(static_cast<TCHAR>(token));
        }
    }
    return text;
}

CString CTaskThread::ExtractPaoShangDestination(const CString& text)
{
    const int length = text.GetLength();
    const auto isOpen = [](TCHAR ch) {
        return ch == _T('(') || ch == static_cast<TCHAR>(0xFF08);
    };
    const auto isClose = [](TCHAR ch) {
        return ch == _T(')') || ch == static_cast<TCHAR>(0xFF09);
    };
    const auto isSeparator = [](TCHAR ch) {
        return ch == _T(',') || ch == static_cast<TCHAR>(0xFF0C);
    };
    const auto isMapChar = [](TCHAR ch) {
        return ch >= static_cast<TCHAR>(0x4E00) &&
               ch <= static_cast<TCHAR>(0x9FFF);
    };

    for (int open = 0; open < length; ++open) {
        if (!isOpen(text[open])) continue;

        int close = open + 1;
        for (; close < length && !isClose(text[close]); ++close) {}
        if (close >= length) break;

        bool hasDigit = false;
        bool hasSeparator = false;
        for (int index = open + 1; index < close; ++index) {
            const TCHAR ch = text[index];
            hasDigit = hasDigit || (ch >= _T('0') && ch <= _T('9'));
            hasSeparator = hasSeparator || isSeparator(ch);
        }
        if (!hasDigit || !hasSeparator) continue;

        int start = open;
        while (start > 0 && isMapChar(text[start - 1])) --start;
        if (start < open) {
            CString destination = text.Mid(start, open - start);
            destination.Trim();
            if (!destination.IsEmpty()) return destination;
        }
        open = close;
    }
    return CString();
}

double CTaskThread::mGetAngle2(Point2f pa, Point2f pb) {
    const double deltaY = static_cast<double>(pb.y) - pa.y;
    const double deltaX = static_cast<double>(pb.x) - pa.x;
    return std::atan2(deltaY, deltaX);
}

bool CTaskThread::UpdateMP(CString cstr, DWORD lon) {
    dwls = Read_Dw(std::move(cstr));
    if (dwls == 0 || !m_dm || !m_ready.load() || !m_wnd || !m_wnd->hWnd) {
        dwls = 0;
        return false;
    }

    const long targetHwnd = static_cast<long>(reinterpret_cast<INT_PTR>(m_wnd->hWnd));
    dwls = static_cast<DWORD>(m_dm->ReadDataAddrToBin(
        targetHwnd, static_cast<LONGLONG>(dwls), static_cast<long>(lon)));
    return dwls != 0;
}

bool CTaskThread::UpdateMP(DWORD paddr, DWORD lon) {
    dwls = Read_Dw(paddr);
    if (dwls == 0 || !m_dm || !m_ready.load() || !m_wnd || !m_wnd->hWnd) {
        dwls = 0;
        return false;
    }

    const long targetHwnd = static_cast<long>(reinterpret_cast<INT_PTR>(m_wnd->hWnd));
    dwls = static_cast<DWORD>(m_dm->ReadDataAddrToBin(
        targetHwnd, static_cast<LONGLONG>(dwls), static_cast<long>(lon)));
    return dwls != 0;
}

void CTaskThread::BianAr(DWORD Erc) {
    constexpr DWORD kSkillTreeStatusOffset = 0x0D;
    constexpr DWORD kSkillTreeLeftOffset = 0x00;
    constexpr DWORD kSkillTreeRightOffset = 0x08;

    if (!m_dm || !m_ready.load() || !m_wnd || !m_wnd->hWnd ||
        Erc < kMinimumUserAddress) {
        return;
    }

    // The skill table is a binary tree. Keep the reference traversal order
    // (node, right child, left child), while stopping on stale/cyclic pointers.
    std::unordered_set<DWORD> visited;
    visited.reserve(128);
    std::function<void(DWORD)> visit = [&](DWORD node) {
        if (node < kMinimumUserAddress || node > 0xFFFFFF00u ||
            visited.size() >= kMaximumStateTreeSteps ||
            !visited.insert(node).second) {
            return;
        }

        const CString state = Dm()->ReadDataAddr(
            mmhwnd, static_cast<LONGLONG>(node + kSkillTreeStatusOffset), 1);
        if (state.CompareNoCase(_T("00")) != 0) return;

        varrou.push_back(node);
        visit(Read_Dw(node + kSkillTreeRightOffset));
        visit(Read_Dw(node + kSkillTreeLeftOffset));
    };

    visit(Erc);
}

int CTaskThread::BianSK(DWORD Erc) {
    constexpr DWORD kSkillTreeFirstOffset = 0x2AE0;
    constexpr DWORD kSkillTreeSecondOffset = 0x2AE0;
    constexpr DWORD kSkillIdOffset = 0x10;
    constexpr DWORD kXinfaLevelOffset = 0x20;
    constexpr DWORD kMenPaiOffset = 0xD0;

    varrou.clear();
    if (!m_dm || !m_ready.load() || !m_wnd || !m_wnd->hWnd) {
        return 0;
    }

    // Resolve the same five-level chain used by the reference project:
    // [[[[[ROLEB]+64]+148]+10]+tableOffset]+4.
    const auto readOffset = [this](DWORD base, DWORD offset) -> DWORD {
        if (base < kMinimumUserAddress ||
            base > 0xFFFFFFFFu - offset) {
            return 0;
        }
        return Read_Dw(base + offset);
    };
    const auto readSkillRoot = [&](DWORD tableOffset) -> DWORD {
        DWORD node = readOffset(
            static_cast<DWORD>(jz[TaskMemoryState::ROLEB]), 0);
        node = readOffset(node, 0x64);
        node = readOffset(node, 0x148);
        node = readOffset(node, 0x10);
        node = readOffset(node, tableOffset);
        return readOffset(node, 0x04);
    };

    // Erc is accepted as an explicit root for diagnostics; zero means use
    // the live character skill table at +0x2A60.
    const DWORD skillRoot = Erc != 0 ? Erc : readSkillRoot(kSkillTreeFirstOffset);
    BianAr(skillRoot);
    const size_t skillNodeCount = varrou.size();

    DWORD menPai = 0;
    bool menPaiRead = false;
    if (!tsg[TaskMemoryState::Shuxtb].IsEmpty() &&
        UpdateMP(tsg[TaskMemoryState::Shuxtb], 0x100) &&
        dwls >= kMinimumUserAddress) {
        // UpdateMP returns a local copy allocated by dmsoft, so this byte must
        // be read from our process rather than through ReadIntAddr again.
        const auto* characterCopy = reinterpret_cast<const BYTE*>(
            static_cast<ULONG_PTR>(dwls));
        menPai = characterCopy[kMenPaiOffset];
        menPaiRead = true;
    }

    bool hasQingGong = false;
    for (const DWORD node : varrou) {
        if (readOffset(node, kSkillIdOffset) == 0x22u) {
            hasQingGong = true;
            break;
        }
    }

    // The old Mams[dqgid] state is not present in this project, so expose the
    // selected ID through the diagnostic log while keeping varrou as the
    // complete first skill tree for callers/tests.
    const DWORD qingGongId = hasQingGong
        ? 0x22u
        : (menPai == 10u ? 0x6Eu : 23u + menPai);
    CString summary;
    summary.Format(
        _T("BianSK: root=0x%08lX nodes=%lu menpai=%s%lu qinggong=0x%08lX"),
        static_cast<unsigned long>(skillRoot),
        static_cast<unsigned long>(skillNodeCount),
        menPaiRead ? _T("") : _T("unknown/"),
        static_cast<unsigned long>(menPai),
        static_cast<unsigned long>(qingGongId));
    DebugLog(summary);

    // Menpai 3 has a second tree containing the 心法 entry (ID 0x15).
    // Scan it for logging, then restore the primary skill tree in varrou.
    if (menPai == 3u) {
        const std::vector<DWORD> skillNodes = varrou;
        varrou.clear();
        const DWORD xinfaRoot = readSkillRoot(kSkillTreeSecondOffset);
        BianAr(xinfaRoot);

        DWORD xinfaLevel = 0;
        bool xinfaFound = false;
        for (const DWORD node : varrou) {
            if (readOffset(node, kSkillIdOffset) == 0x15u) {
                xinfaLevel = readOffset(node, kXinfaLevelOffset);
                xinfaFound = true;
                break;
            }
        }

        DebugLog(_T("BianSK: xinfa root=0x%08lX nodes=%lu level=%s%lu"),
                 static_cast<unsigned long>(xinfaRoot),
                 static_cast<unsigned long>(varrou.size()),
                 xinfaFound ? _T("") : _T("unknown/"),
                 static_cast<unsigned long>(xinfaLevel));
        varrou = skillNodes;
    } else if (menPai == 4u) {
        // The reference reads RWQX here; the current framework has no blp/
        // ReadIni state yet, so retain the branch as an explicit trace point.
        DebugLog(_T("BianSK: menpai=4, RWQX state is not mapped"));
    }

    return static_cast<int>(skillNodeCount);
}

void CTaskThread::SleepEx(DWORD milliseconds) {
    constexpr DWORD step = 100;

    if (milliseconds < step) {
        if (m_control->IsStopping()) return;
        ::Sleep(milliseconds);
        (void)WaitIfPaused();
        return;
    }

    DWORD elapsed = 0;
    while (elapsed + step <= milliseconds) {
        if (m_control->IsStopping()) return;

        ::Sleep(step);
        elapsed += step;
        if (!WaitIfPaused()) return;
    }

    if (m_control->IsStopping()) return;
    ::Sleep(milliseconds - elapsed);
    (void)WaitIfPaused();
}

void CTaskThread::ThreadMain() {
    const DWORD threadId = GetCurrentThreadId();
    m_threadId.store(threadId);
    DebugLog(_T("start"));
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool comInitialized = SUCCEEDED(comResult);
    if (!comInitialized) DebugLog(_T("CoInitializeEx failed"));

    if (comInitialized) {
        m_dm = std::make_unique<dmsoft>();
        m_dm->m_pWnd = m_wnd.get();
        m_dm->SetShowErrorMsg(0);
        m_ready.store(m_dm != nullptr);
        DebugLog(m_ready.load() ? _T("dmsoft created") : _T("dmsoft creation failed"));
        if (m_dm && OnEventThreadStart()) {
            while (OnEventThreadRun()) {
            }
        }
    }

    OnEventThreadConclude();
    m_ready.store(false);
    m_dm.reset();
    if (comInitialized) CoUninitialize();
    DebugLog(_T("stop / released"));
    m_running.store(false);
}

CPoint CTaskThread::ToScreen(int x, int y) const {
    POINT point{ x, y };
    if (m_wnd && m_wnd->hWnd) ::ClientToScreen(m_wnd->hWnd, &point);
    return CPoint(point);
}

CRect CTaskThread::ToScreen(const CRect& area) const {
    CPoint lt = ToScreen(area.left, area.top);
    CPoint rb = ToScreen(area.right, area.bottom);
    return CRect(lt, rb);
}

void CTaskThread::havetest()
{
    CString message;
    message.Format(_T("havetest: jz[ROLEB] = 0x%llX"),
                   static_cast<unsigned long long>(jz[ROLEB]));
    DebugLog(message);
}

int CTaskThread::wBenTuXuL2(float xx, float yy)
{
    Dm()->WriteFloatAddr(mmhwnd, jz[TaskMemoryState::Nbsq] + 480 + 34, xx);
    Dm()->WriteFloatAddr(mmhwnd, jz[TaskMemoryState::Nbsq] + 480 + 27, yy);
    Dm()->WriteIntAddr(mmhwnd, jz[TaskMemoryState::Nbsq] + ZHUTC, 0, jz[TaskMemoryState::Nbsq] + 480);//标准

    Dm()->WriteIntAddr(mmhwnd, jz[TaskMemoryState::Nbsq] + ZHUTB, 0, 1);//标志
    return 0;
}

int CTaskThread::wOpenMan(DWORD did)
{
    Dm()->WriteIntAddr(mmhwnd, jz[TaskMemoryState::Nbsq] + 530 + 9, 0, did);
    Dm()->WriteIntAddr(mmhwnd, jz[TaskMemoryState::Nbsq] + ZHUBC, 0, jz[TaskMemoryState::Nbsq] + 530);
    Dm()->WriteIntAddr(mmhwnd, jz[TaskMemoryState::Nbsq] + ZHUBB, 0, 1);
    return 0;
}

void CTaskThread::wXuanZg(DWORD moid)
{
	Dm()->WriteIntAddr(mmhwnd, jz[TaskMemoryState::Nbsq] + 560 + 9, 0, moid);
	Dm()->WriteIntAddr(mmhwnd, jz[TaskMemoryState::Nbsq] + ZHUBC, 0, jz[TaskMemoryState::Nbsq] + 560);
    Dm()->WriteIntAddr(mmhwnd, jz[TaskMemoryState::Nbsq] + ZHUBB, 0, 1);
}

bool CTaskThread::tMoveTo(int x, int y) {
    return m_dm && m_ready.load() && m_dm->MoveTo(ToScreen(x, y).x, ToScreen(x, y).y) != 0;
}

bool CTaskThread::tClick(int x, int y) {
    if (!tMoveTo(x, y)) return false;
    return m_dm->LeftClick() != 0;
}

bool CTaskThread::trClick(int x, int y) {
    if (!tMoveTo(x, y)) return false;
    return m_dm->RightClick() != 0;
}

bool CTaskThread::FindPic(const CRect& area, const CString& file, CPoint& point,
                          const CString& deltaColor, double sim, int dir) {
    if (!m_dm || !m_ready.load()) return false;
    const CRect r = ToScreen(area);
    long x = 0, y = 0;
    const long result = m_dm->FindPic(r.left, r.top, r.right, r.bottom, file.GetString(),
                                      deltaColor.GetString(), sim, dir, &x, &y);
    if (result == 0) return false;
    point = CPoint(static_cast<int>(x), static_cast<int>(y));
    return true;
}

bool CTaskThread::FindColor(const CRect& area, const CString& color, CPoint& point, double sim, int dir) {
    if (!m_dm || !m_ready.load()) return false;
    const CRect r = ToScreen(area);
    long x = 0, y = 0;
    const long result = m_dm->FindColor(r.left, r.top, r.right, r.bottom, color.GetString(), sim, dir, &x, &y);
    if (result == 0) return false;
    point = CPoint(static_cast<int>(x), static_cast<int>(y));
    return true;
}

long CTaskThread::GetColorNum(const CRect& area, const CString& color, double sim) {
    if (!m_dm || !m_ready.load()) return 0;
    const CRect r = ToScreen(area);
    return m_dm->GetColorNum(r.left, r.top, r.right, r.bottom, color.GetString(), sim);
}

namespace {

constexpr DWORD kNearbyTreeStatusOffset = 13;
constexpr DWORD kNearbyTreeEntityOffset = 20;
constexpr DWORD kNearbyTreeLeftOffset = 8;
constexpr DWORD kNearbyTreeRightOffset = 0;
constexpr DWORD kNearbyDescriptorCopyLength = 0x100;
constexpr DWORD kNearbyEntityCopyLength = 0x200;
constexpr DWORD kNearbyNameOffset = 0x2C;
constexpr DWORD kNearbyHpOffset = 0x08;
constexpr DWORD kNearbyXOffset = 0x54;
constexpr DWORD kNearbyYOffset = 0x5C;
constexpr DWORD kNearbyTrapXOffset = 0x3C;
constexpr DWORD kNearbyTrapYOffset = 0x44;

CString BuildNearbyDescriptorExpression(DWORD node) {
    CString expression;
    expression.Format(_T("[[%08lX]+148]+10"),
                      static_cast<unsigned long>(node));
    return expression;
}

CString BuildNearbyNameExpression(DWORD node) {
    CString expression;
    expression.Format(_T("[[[%08lX]+148]+10]+2C"),
                      static_cast<unsigned long>(node));
    return expression;
}

CString BuildNearbyTrapNameExpression(DWORD node) {
    CString expression;
    expression.Format(_T("[[[%08lX]+110]+4]"),
                      static_cast<unsigned long>(node));
    return expression;
}

CString BuildNearbyTitleExpression(DWORD node) {
    CString expression;
    expression.Format(_T("[[[%08lX]+148]+10]+6C"),
                      static_cast<unsigned long>(node));
    return expression;
}

std::string CStringToNearbyAnsi(const CString& text) {
#ifdef _UNICODE
    if (text.IsEmpty()) return {};
    const int sourceLength = text.GetLength();
    const int byteLength = ::WideCharToMultiByte(
        CP_ACP, 0, text.GetString(), sourceLength, nullptr, 0, nullptr, nullptr);
    if (byteLength <= 0) return {};

    std::string result(static_cast<size_t>(byteLength), '\0');
    ::WideCharToMultiByte(CP_ACP, 0, text.GetString(), sourceLength,
                          result.data(), byteLength, nullptr, nullptr);
    return result;
#else
    return std::string(text.GetString());
#endif
}

CString NearbyAnsiBufferToCString(DWORD buffer, DWORD offset = 0,
                                   DWORD maxLength = 256) {
    if (buffer == 0) return {};

    const char* source = reinterpret_cast<const char*>(
        static_cast<ULONG_PTR>(buffer) + static_cast<ULONG_PTR>(offset));
    DWORD length = 0;
    while (length < maxLength && source[length] != '\0') ++length;
    if (length == 0) return {};

#ifdef _UNICODE
    const int targetLength = ::MultiByteToWideChar(
        CP_ACP, 0, source, static_cast<int>(length), nullptr, 0);
    if (targetLength <= 0) return {};

    CString result;
    wchar_t* destination = result.GetBuffer(targetLength);
    ::MultiByteToWideChar(CP_ACP, 0, source, static_cast<int>(length),
                          destination, targetLength);
    result.ReleaseBuffer(targetLength);
    return result;
#else
    return CString(source, static_cast<int>(length));
#endif
}

template <typename TValue>
bool ReadNearbyLocalValue(DWORD buffer, DWORD offset, TValue& value) {
    if (buffer == 0) return false;
    const auto* source = reinterpret_cast<const BYTE*>(
        static_cast<ULONG_PTR>(buffer) + static_cast<ULONG_PTR>(offset));
    std::memcpy(&value, source, sizeof(TValue));
    return true;
}

bool IsNearbyChineseText(const CString& text) {
    for (int index = 0; index < text.GetLength(); ++index) {
        const wchar_t character = text[index];
        if (character >= 0x4E00 && character <= 0x9FFF) return true;
    }
    return false;
}

Monst MakeNearbyMonster(int id, DWORD node, float distance,
                        const CString& descriptorExpression,
                        const CString& name, const Point2f& position) {
    Monst result;
    result.ID = id;
    result.Dibu = node;
    result.Juli = distance;
    result.cxue = descriptorExpression;
    result.mname = name;
    result.pos = position;
    return result;
}

} // namespace

regex CTaskThread::NerbCs(float fjl, Point2f pwz, CString txt_reg) {
    m_nearbyDistanceLimit = fjl;
    m_nearbyReferencePosition = pwz;
    m_nearbyReferencePositionValid = true;

    try {
        return regex(CStringToNearbyAnsi(txt_reg));
    } catch (const std::regex_error&) {
        DebugLog(_T("nearby regex is invalid; returning a non-matching expression"));
        return regex("a^");
    }
}

bool CTaskThread::ReadNearbyPlayerPosition(Point2f& position) {
    if (!m_dm || !m_ready.load() || !m_wnd || !m_wnd->hWnd ||
        tsg[TaskMemoryState::Shuxtb].IsEmpty()) {
        return false;
    }

    if (!UpdateMP(tsg[TaskMemoryState::Shuxtb], 0x100)) return false;

    const DWORD playerCopy = dwls;
    return ReadNearbyLocalValue(playerCopy, kNearbyXOffset, position.x) &&
           ReadNearbyLocalValue(playerCopy, kNearbyYOffset, position.y);
}

int CTaskThread::IsWndGuot()
{
    if (!m_dm || !m_ready.load() || !m_wnd || !m_wnd->hWnd) {
        return 0;
    }

    // SceneTrans_Frame + 0xE5 is the transition-window display byte.
    // PlayerFrame_Icon + 0x4E0 stores the avatar image/data pointer; during
    // a map transition this field is cleared.  Both conditions must hold.
    const DWORD sceneWindow = static_cast<DWORD>(ck[SceneTrans_Frame]);
    const DWORD iconWindow = static_cast<DWORD>(ck[PlayerFrame_Icon]);
    if (sceneWindow == 0 || iconWindow == 0) {
        return 0;
    }

    const long targetHwnd = static_cast<long>(reinterpret_cast<INT_PTR>(m_wnd->hWnd));
    const BYTE transitionShown = static_cast<BYTE>(m_dm->ReadIntAddr(
        targetHwnd, static_cast<LONGLONG>(sceneWindow) + 0xE5, 2));
    const DWORD avatarData = static_cast<DWORD>(m_dm->ReadIntAddr(
        targetHwnd, static_cast<LONGLONG>(iconWindow) + 0x4E0, 4));

    return transitionShown != 0 || avatarData == 0 ? 1 : 0;
}

DWORD CTaskThread::cGetTime()
{
    if (!m_dm || !m_ready.load() || !m_wnd || !m_wnd->hWnd) {
        return MAXDWORD;
    }

    const DWORD window = static_cast<DWORD>(ck[Frame_Time]);
    if (window == 0) return MAXDWORD;

    // TLBB_MapImage 当前使用的 CEGUI::Image* 保存在 Window + 0x4E0。
    const DWORD image = Read_Dw(window + 0x4E0);
    if (image == 0) return MAXDWORD;

    // CEGUI::Image + 0x2C 是图片名 std::string。
    const DWORD nameObject = image + 0x2C;
    const DWORD length = Read_Dw(nameObject + 0x10);
    const DWORD capacity = Read_Dw(nameObject + 0x14);
    if (length == 0 || length > capacity || length > 0x100) return MAXDWORD;

    DWORD nameAddress = nameObject;
    if (capacity >= 0x10) {
        nameAddress = Read_Dw(nameObject);
        if (nameAddress == 0) return MAXDWORD;
    }

    const long targetHwnd = static_cast<long>(reinterpret_cast<INT_PTR>(m_wnd->hWnd));
    const CString imageName = m_dm->ReadStringAddr(
        targetHwnd, static_cast<LONGLONG>(nameAddress), 0,
        static_cast<long>(length));
    const CString prefix(_T("Minimap_Time"));
    if (imageName.GetLength() <= prefix.GetLength() ||
        imageName.Left(prefix.GetLength()).CompareNoCase(prefix) != 0) {
        return MAXDWORD;
    }

    const CString numberText = imageName.Mid(prefix.GetLength());
    for (int index = 0; index < numberText.GetLength(); ++index) {
        if (numberText[index] < _T('0') || numberText[index] > _T('9')) {
            return MAXDWORD;
        }
    }

    const int imageIndex = _ttoi(numberText);
    return imageIndex >= 1 && imageIndex <= 12
        ? static_cast<DWORD>(imageIndex - 1)
        : MAXDWORD;
}

DWORD CTaskThread::cGetNetIc()
{
    if (!m_dm || !m_ready.load() || !m_wnd || !m_wnd->hWnd) return 0;

    const DWORD window = static_cast<DWORD>(ck[MiniMap_MiniMap_NetStatus]);
    if (window == 0) return 0;

    const DWORD image = Read_Dw(window + 0x4E0);
    if (image == 0) return 0;

    const DWORD nameObject = image + 0x2C;
    const DWORD length = Read_Dw(nameObject + 0x10);
    const DWORD capacity = Read_Dw(nameObject + 0x14);
    if (length == 0 || length > capacity || length > 0x100) return 0;

    DWORD nameAddress = nameObject;
    if (capacity >= 0x10) {
        nameAddress = Read_Dw(nameObject);
        if (nameAddress == 0) return 0;
    }

    const long targetHwnd = static_cast<long>(reinterpret_cast<INT_PTR>(m_wnd->hWnd));
    const CString imageName = m_dm->ReadStringAddr(
        targetHwnd, static_cast<LONGLONG>(nameAddress), 0,
        static_cast<long>(length));
    const CString prefix(_T("Minimap_NetState"));
    if (imageName.GetLength() != prefix.GetLength() + 1 ||
        imageName.Left(prefix.GetLength()).CompareNoCase(prefix) != 0) {
        return 0;
    }

    const TCHAR stateCharacter = imageName[imageName.GetLength() - 1];
    if (stateCharacter < _T('1') || stateCharacter > _T('4')) return 0;
    return static_cast<DWORD>(stateCharacter - _T('0'));
}

int CTaskThread::cRead_Identify(DWORD xuh)
{
    if (!m_dm || !m_ready.load() || !m_wnd || !m_wnd->hWnd) return 0;

    const DWORD bagSlot = static_cast<DWORD>(jz[TaskMemoryState::BagJ]);
    if (bagSlot == 0) return 0;

    const DWORD bagBase = Read_Dw(bagSlot);
    if (bagBase == 0) return 0;

    const DWORD itemTable = Read_Dw(bagBase + 0x22C00);
    if (itemTable == 0) return 0;

    const DWORD item = Read_Dw(itemTable + xuh * sizeof(DWORD));
    if (item == 0) return 0;

    // Match the old LifeAbility:IsIndentify(index) read-only path:
    // item->data->type 8/9/10 is not an identifiable equipment item.
    const DWORD data = Read_Dw(item + 0x2C);
    if (data == 0) return 0;

    const DWORD type = Read_Dw(data + 0x14);
    if (type == 8 || type == 9 || type == 10) return 0;

    const DWORD ext = Read_Dw(item + 0x1C);
    if (ext == 0 || Read_Dw(ext + 0x28) == 0) return 0;

    const DWORD state = Read_Dw(item + 0x14);
    if (state == 0) return 0;

    if (static_cast<BYTE>(Read_Dw(state + 0x5E)) == 0) return 0;

    // 01084360 returns (state->flagsByte & 0x02) >> 1.  The caller inverts
    // that result: 1 means unidentified and 0 means identified.
    return (Read_Dw(state + 0x0D) & 0x02) != 0 ? 0 : 1;
}

int CTaskThread::cRead_Bind(DWORD xuh)
{
    if (!m_dm || !m_ready.load() || !m_wnd || !m_wnd->hWnd) return 0;

    const DWORD bagSlot = static_cast<DWORD>(jz[TaskMemoryState::BagJ]);
    if (bagSlot == 0) return 0;

    const DWORD bagBase = Read_Dw(bagSlot);
    if (bagBase == 0) return 0;

    const DWORD itemTable = Read_Dw(bagBase + 0x22C00);
    if (itemTable == 0) return 0;

    const DWORD item = Read_Dw(itemTable + xuh * sizeof(DWORD));
    if (item == 0) return 0;

    const DWORD state = Read_Dw(item + 0x14);
    if (state == 0) return 0;

    // 00ED5940 first tests the low bit of state+0x0D.  This is the raw
    // already-bound result and must be checked before the two item-property
    // branches below.
    if ((Read_Dw(state + 0x0D) & 0x01) != 0) return 1;

    const DWORD ext = Read_Dw(item + 0x1C);
    // vtable+0x98(6) is exactly:
    //   ext == 0 || [ext+0x1C] != 0
    // If true, the original calls vtable+0x28, which is item+0x0C, then
    // returns 1 when (item+0x0C - 2) <= 1 and 2 otherwise.
    const bool property6 = ext == 0 || Read_Dw(ext + 0x1C) != 0;
    if (property6) {
        const DWORD categoryMinusTwo = Read_Dw(item + 0x0C) - 2;
        return categoryMinusTwo > 1 ? 2 : 1;
    }

    // vtable+0x98(7) is ext == 0 || [ext+0x20] != 0.  This is the final
    // boolean-to-code path: false -> 0, true -> 3.
    const bool property7 = ext == 0 || Read_Dw(ext + 0x20) != 0;
    return property7 ? 3 : 0;
}

CString CTaskThread::cRead_CeguiText(DWORD xuh)
{
    if (static_cast<size_t>(xuh) >= Fruit15Count) {
        return CString();
    }

    const DWORD window = static_cast<DWORD>(ck[xuh]);
    return ReadCeguiText(window);
}

DWORD CTaskThread::mRead_Benj()
{
    if (!m_dm || !m_ready.load() || !m_wnd || !m_wnd->hWnd) {
        DebugLog(_T("mRead_Benj: precondition failed"));
        return 0;
    }

    const DWORD moneyWindow = FindCeguiWindowByName(
        _T("PaoShang_Money4_Need"));
    const DWORD windowMoney = ReadCeguiNumericText(moneyWindow);
    CString trace;
    trace.Format(_T("mRead_Benj: moneyWindow=0x%08lX value=%lu"),
                 static_cast<unsigned long>(moneyWindow),
                 static_cast<unsigned long>(windowMoney));
    DebugLog(trace);
    if (windowMoney != 0) return windowMoney;

    if (jz[TaskMemoryState::RenW] == 0) {
        DebugLog(_T("mRead_Benj: no window value and RenW is null"));
        return 0;
    }

    // RenW is the DataPool object.  Its +0x28 field points to the mission
    // records; each record is 0x29 bytes and the mission id is at +0x09.
    const DWORD dataPool = static_cast<DWORD>(jz[TaskMemoryState::RenW]);
    const DWORD missionTable = Read_Dw(dataPool + kMissionTableOffset);
    trace.Format(_T("mRead_Benj: RenW=0x%08lX table=0x%08lX"),
                 static_cast<unsigned long>(dataPool),
                 static_cast<unsigned long>(missionTable));
    DebugLog(trace);
    if (missionTable == 0) return 0;

    for (int index = 0; index < kMissionRecordCount; ++index) {
        const DWORD record = missionTable +
            static_cast<DWORD>(index) * kMissionRecordStride;
        const DWORD missionId = Read_Dw(record + kMissionIdOffset);
        if (missionId == kPaoShangMissionId) {
            const DWORD money = Read_Dw(record + kPaoShangZijinOffset);
            trace.Format(_T("mRead_Benj: hit index=%d record=0x%08lX id=%lu money=%lu"),
                         index, static_cast<unsigned long>(record),
                         static_cast<unsigned long>(missionId),
                         static_cast<unsigned long>(money));
            DebugLog(trace);
            return money;
        }
    }

    DebugLog(_T("mRead_Benj: mission 2115 not found"));
    return 0;
}

DWORD CTaskThread::mRead_TodayActive()
{
    if (!m_dm || !m_ready.load() || !m_wnd || !m_wnd->hWnd) {
        DebugLog(_T("mRead_TodayActive: precondition failed"));
        return 0;
    }

    DWORD conditionWindow = static_cast<DWORD>(ck[ZhouHuoYue_Condition_Text]);
    if (conditionWindow == 0) {
        conditionWindow = FindCeguiWindowByName(
            _T("ZhouHuoYue_Condition_Text"));
        if (conditionWindow != 0) {
            ck[ZhouHuoYue_Condition_Text] = conditionWindow;
        }
    }
    if (conditionWindow == 0) {
        DebugLog(_T("mRead_TodayActive: condition window not found"));
        return 0;
    }

    // The Lua handler receives today's activity as arg4 and formats it into
    // "...:current/max". Read the rendered token buffer directly and use the
    // last numeric run before the slash so the max value is never returned.
    const CString text = ReadCeguiText(conditionWindow);
    if (text.IsEmpty()) {
        DebugLog(_T("mRead_TodayActive: condition text is empty"));
        return 0;
    }

    int end = text.GetLength();
    for (int index = 0; index < text.GetLength(); ++index) {
        if (text[index] == _T('/') || text[index] == static_cast<TCHAR>(0xFF0F)) {
            end = index;
            break;
        }
    }

    DWORD current = 0;
    DWORD candidate = 0;
    bool inDigits = false;
    bool found = false;
    for (int index = 0; index < end; ++index) {
        const TCHAR character = text[index];
        if (character >= _T('0') && character <= _T('9')) {
            const DWORD digit = static_cast<DWORD>(character - _T('0'));
            if (!inDigits) candidate = 0;
            if (candidate > (0xFFFFFFFFUL - digit) / 10UL) {
                DebugLog(_T("mRead_TodayActive: numeric token overflow"));
                return 0;
            }
            candidate = candidate * 10UL + digit;
            inDigits = true;
            continue;
        }

        if (inDigits) {
            current = candidate;
            found = true;
            inDigits = false;
        }
    }
    if (inDigits) {
        current = candidate;
        found = true;
    }

    CString trace;
    trace.Format(_T("mRead_TodayActive: window=0x%08lX text=%s value=%lu"),
                 static_cast<unsigned long>(conditionWindow), text.GetString(),
                 static_cast<unsigned long>(found ? current : 0));
    DebugLog(trace);
    return found ? current : 0;
}

PaoShangRecommendData CTaskThread::mRead_TuiJg()
{
    PaoShangRecommendData result;
    if (!m_dm || !m_ready.load() || !m_wnd || !m_wnd->hWnd) {
        DebugLog(_T("mRead_TuiJg: precondition failed"));
        return result;
    }

    const long targetHwnd = static_cast<long>(reinterpret_cast<INT_PTR>(m_wnd->hWnd));
    const auto resolveWindow = [this](Fruit15 slot, LPCTSTR name) -> DWORD {
        DWORD window = static_cast<DWORD>(ck[slot]);
        if (window == 0) {
            window = FindCeguiWindowByName(name);
            if (window != 0) ck[slot] = window;
        }
        return window;
    };

    const DWORD paoshangFrame = resolveWindow(
        PaoShang_Frame, _T("PaoShang_Frame"));
    CString trace;
    trace.Format(_T("mRead_TuiJg: frame=0x%08lX"),
                 static_cast<unsigned long>(paoshangFrame));
    DebugLog(trace);
    if (paoshangFrame == 0) return result;

    auto frameVisible = [&]() -> bool {
        return static_cast<BYTE>(m_dm->ReadIntAddr(
            targetHwnd, static_cast<LONGLONG>(paoshangFrame) + 0xE5, 1)) != 0;
    };
    if (!frameVisible()) {
        DebugLog(_T("mRead_TuiJg: frame hidden"));
        Sleep(1000);
        if (!frameVisible()) {
            DebugLog(_T("mRead_TuiJg: frame still hidden"));
            return result;
        }
    }

    const Fruit15 markerSlots[] = {
        PaoShang_Goumai_Tuijian1,
        PaoShang_Goumai_Tuijian2,
        PaoShang_Goumai_Tuijian3
    };
    const Fruit15 priceSlots[] = {
        PaoShang_ItemInfo1_Price,
        PaoShang_ItemInfo2_Price,
        PaoShang_ItemInfo3_Price
    };
    const LPCTSTR markerNames[] = {
        _T("PaoShang_Goumai_Tuijian1"),
        _T("PaoShang_Goumai_Tuijian2"),
        _T("PaoShang_Goumai_Tuijian3")
    };

    int recommended = -1;
    for (int index = 0; index < 3; ++index) {
        const DWORD marker = resolveWindow(markerSlots[index], markerNames[index]);
        if (marker == 0) {
            trace.Format(_T("mRead_TuiJg: marker%d=0"), index + 1);
            DebugLog(trace);
            continue;
        }

        const BYTE visible = static_cast<BYTE>(m_dm->ReadIntAddr(
            targetHwnd, static_cast<LONGLONG>(marker) + 0xE5, 1));
        trace.Format(_T("mRead_TuiJg: marker%d=0x%08lX visible=%u"),
                     index + 1, static_cast<unsigned long>(marker), visible);
        DebugLog(trace);
        if (visible != 0) {
            recommended = index;
            break;
        }
    }

    if (recommended < 0) {
        DebugLog(_T("mRead_TuiJg: no recommended item"));
        return result;
    }

    const LPCTSTR priceNames[] = {
        _T("PaoShang_ItemInfo1_Price"),
        _T("PaoShang_ItemInfo2_Price"),
        _T("PaoShang_ItemInfo3_Price")
    };
    const DWORD priceWindow = resolveWindow(
        priceSlots[recommended], priceNames[recommended]);
    trace.Format(_T("mRead_TuiJg: recommended=%d priceWindow=0x%08lX"),
                 recommended + 1, static_cast<unsigned long>(priceWindow));
    DebugLog(trace);
    if (priceWindow == 0) return result;

    const DWORD price = ReadCeguiNumericText(priceWindow);
    trace.Format(_T("mRead_TuiJg: encoded price=%lu"),
                 static_cast<unsigned long>(price));
    DebugLog(trace);
    if (price == 0) {
        DebugLog(_T("mRead_TuiJg: price text not found"));
        return result;
    }

    const LPCTSTR buttonNames[] = {
        _T("PaoShang_But1"),
        _T("PaoShang_But2"),
        _T("PaoShang_But3")
    };

    result.price = price;

    // TLBB_ActionButton stores the item/action ID at +0x5B4.
    const DWORD buttonWindow = FindCeguiWindowByName(buttonNames[recommended]);
    if (buttonWindow != 0) {
        result.id = Read_Dw(buttonWindow + 0x5B4);
    }

    // The destination is the map name at the beginning of the tooltip's
    // coordinate line, for example "太湖（180，116）".  Different item
    // sources use one of the four tooltip instances below.  Read the text
    // even when the root has just been hidden: the game clears the visible
    // flag before it clears the token buffer, which otherwise loses the last
    // valid destination during a test-button click.
    struct TooltipCandidate {
        LPCTSTR rootName;
        LPCTSTR explainName;
    };
    const TooltipCandidate tooltipCandidates[] = {
        { _T("_SuperTooltip_"),       _T("SuperTooltip_Explain") },
        { _T("_SuperTooltip2_"),      _T("SuperTooltip2_Explain") },
        { _T("_DressSuperToolTip_"),  _T("DressSuperToolTip_Explain") },
        { _T("_DressSuperToolTip2_"), _T("DressSuperToolTip2_Explain") }
    };

    CString fallbackDestination;
    for (const TooltipCandidate& candidate : tooltipCandidates) {
        const DWORD tooltipRoot = FindCeguiWindowByName(candidate.rootName);
        if (tooltipRoot == 0) continue;

        const BYTE tooltipVisible = static_cast<BYTE>(m_dm->ReadIntAddr(
            targetHwnd, static_cast<LONGLONG>(tooltipRoot) + 0xE5, 1));
        const DWORD explainWindow = FindCeguiWindowByName(candidate.explainName);
        const CString tooltipText = ReadCeguiText(explainWindow);
        const CString destination = ExtractPaoShangDestination(tooltipText);

        trace.Format(_T("mRead_TuiJg: tooltip=%s root=0x%08lX visible=%u explain=0x%08lX textLen=%d destination=%s"),
                     candidate.rootName,
                     static_cast<unsigned long>(tooltipRoot),
                     tooltipVisible,
                     static_cast<unsigned long>(explainWindow),
                     tooltipText.GetLength(),
                     destination.GetString());
        DebugLog(trace);

        if (!destination.IsEmpty() && fallbackDestination.IsEmpty()) {
            fallbackDestination = destination;
        }
        if (tooltipVisible != 0 && !destination.IsEmpty()) {
            result.destination = destination;
            break;
        }
    }
    if (result.destination.IsEmpty()) {
        result.destination = fallbackDestination;
    }

    trace.Format(_T("mRead_TuiJg: price=%lu id=%lu destination=%s"),
                 static_cast<unsigned long>(result.price),
                 static_cast<unsigned long>(result.id),
                 result.destination.GetString());
    DebugLog(trace);
    return result;
}

DWORD CTaskThread::mRead_BangZw()
{
    if (!m_dm || !m_ready.load() || !m_wnd || !m_wnd->hWnd || !m_memory) {
        DebugLog(_T("mRead_BangZw: precondition failed"));
        return 0;
    }

    const long targetHwnd = static_cast<long>(reinterpret_cast<INT_PTR>(m_wnd->hWnd));
    // Resolve the live module base first; the cached value can be stale after
    // the game logs in again or reloads the client image.
    DWORD gameModule = static_cast<DWORD>(m_dm->GetModuleBaseAddr(
        targetHwnd, _T("Game.exe")));
    if (gameModule == 0 && m_memory) {
        gameModule = static_cast<DWORD>(m_memory->gameModule);
    }
    if (gameModule == 0) {
        DebugLog(_T("mRead_BangZw: Game.exe module not found"));
        return 0;
    }

    const DWORD playerData = static_cast<DWORD>(m_dm->ReadIntAddr(
        targetHwnd, static_cast<LONGLONG>(gameModule) + kGuildPlayerGlobalRva, 4));
    if (playerData == 0) {
        DebugLog(_T("mRead_BangZw: player data pointer is null"));
        return 0;
    }

    const DWORD rank = static_cast<BYTE>(m_dm->ReadIntAddr(
        targetHwnd, static_cast<LONGLONG>(playerData) + kGuildRankOffset, 1));
    CString trace;
    trace.Format(_T("mRead_BangZw: global=0x%08lX data=0x%08lX rank=%lu"),
                 static_cast<unsigned long>(static_cast<LONGLONG>(gameModule) +
                                             kGuildPlayerGlobalRva),
                 static_cast<unsigned long>(playerData),
                 static_cast<unsigned long>(rank));
    DebugLog(trace);

    // The original switch subtracts two and accepts eight cases (2..9).
    return rank >= 2 && rank <= 9 ? rank : 0;
}

CString CTaskThread::Read_NpcT(int xuh)
{
    if (xuh < 0 || !m_dm || !m_ready.load() ||
        !m_wnd || !m_wnd->hWnd) {
        return CString();
    }

    // The old ReadString expression is equivalent to:
    //   dataPool = [RenW]
    //   npcTextBase = [dataPool + 0x2C]
    //   text = npcTextBase + xuh * 0x118 + 0x16
    // Resolve those dwords explicitly so a stale/invalid pointer fails
    // cleanly instead of being hidden by the expression parser.
    constexpr DWORD kNpcDataPoolOffset = 0x2C;
    constexpr DWORD kNpcRecordStride = 0x118;
    constexpr DWORD kNpcTextOffset = 0x16;

    const DWORD roleWork = static_cast<DWORD>(jz[TaskMemoryState::RenW]);
    if (roleWork < kMinimumUserAddress) return CString();

    const auto readDword = [this](DWORD address) -> DWORD {
        return static_cast<DWORD>(Dm()->ReadIntAddr(
            mmhwnd, static_cast<LONGLONG>(address), 4));
    };

    const DWORD dataPool = readDword(roleWork);
    if (dataPool < kMinimumUserAddress) return CString();

    const DWORD npcTextBase = readDword(dataPool + kNpcDataPoolOffset);
    if (npcTextBase < kMinimumUserAddress) return CString();

    const LONGLONG textAddress = static_cast<LONGLONG>(npcTextBase) +
                                  static_cast<LONGLONG>(xuh) * kNpcRecordStride +
                                  kNpcTextOffset;
    // type=0 is the game's GBK/code-page text. length=0 lets dmsoft detect
    // the terminating NUL, so the text is not truncated to a fixed buffer.
    return Dm()->ReadStringAddr(mmhwnd, textAddress, 0, 0);
}

//---------------------宠物+品----------------------
int CTaskThread::Read_PetMx()
{
    constexpr DWORD kPetRecordCount = 7;
    constexpr DWORD kPetAttackOffset1 = 196;
    constexpr DWORD kPetAttackOffset2 = 184;
    constexpr DWORD kPetSoulIdOffset = 8;

    if (!m_dm || !m_ready.load() || !m_wnd || !m_wnd->hWnd ||
        tsg[TaskMemoryState::shuxcw].IsEmpty()) {
        return -1;
    }

    UpdateMP(tsg[TaskMemoryState::shuxcw], 0x1000);
    if (dwls < kMinimumUserAddress) return -1;

    const auto* petBase = reinterpret_cast<const BYTE*>(
        static_cast<ULONG_PTR>(dwls));
    int maxAttack = -1;
    int selected = -1;

    for (DWORD i = 0; i < kPetRecordCount; ++i) {
        const BYTE* record = petBase + static_cast<size_t>(i) * kPetRecordSize;
        const DWORD recordState = *reinterpret_cast<const DWORD*>(record);
        if (recordState == 0xFFFFFFFFu) break;

        if (Read_CWCZ(static_cast<int>(i))) {
            return static_cast<int>(i);
        }

        const int attack1 = static_cast<int>(
            *reinterpret_cast<const DWORD*>(record + kPetAttackOffset1));
        const int attack2 = static_cast<int>(
            *reinterpret_cast<const DWORD*>(record + kPetAttackOffset2));
        const int attack = (std::max)(attack1, attack2);
        if (attack <= maxAttack) continue;

        const DWORD petSoulId = *reinterpret_cast<const DWORD*>(
            record + kPetSoulIdOffset);
        CString expression;
        expression.Format(_T("[[[[%08x]+64]+148]+10]+%08x"),
                           static_cast<DWORD>(jz[TaskMemoryState::ROLEB]),
                           static_cast<DWORD>(kPetSoulOffset));
        const DWORD roleSoulId = static_cast<DWORD>(
            Dm()->ReadInt(mmhwnd, expression, 4));
        if (petSoulId == roleSoulId) continue;

        maxAttack = attack;
        selected = static_cast<int>(i);
        Mams[Cox1] = *reinterpret_cast<const DWORD*>(record + 8);
        Mams[Cox2] = *reinterpret_cast<const DWORD*>(record + 12);
    }

    return maxAttack == -1 ? -1 : selected;
}

bool CTaskThread::Read_CWCZ(int xuh)
{
    constexpr DWORD kPetRecordCount = 7;
    constexpr DWORD kPetStatusOffset1 = 8;
    constexpr DWORD kPetStatusOffset2 = 12;
    constexpr DWORD kPetStatusOffset3 = 4;

    if (!m_dm || !m_ready.load() || !m_wnd || !m_wnd->hWnd ||
        xuh < 0 || static_cast<DWORD>(xuh) >= kPetRecordCount) {
        return false;
    }

    const DWORD bagJ = static_cast<DWORD>(jz[TaskMemoryState::BagJ]);
    const DWORD roleBase = static_cast<DWORD>(jz[TaskMemoryState::ROLEB]);
    if (bagJ < kMinimumUserAddress || roleBase < kMinimumUserAddress) {
        return false;
    }

    const DWORD petOffset = static_cast<DWORD>(xuh) * kPetRecordSize;
    CString expression;
    expression.Format(_T("[[%08x]+%08x]+%08x"),
                       bagJ, kPetTableOffset, petOffset + kPetStatusOffset1);
    const DWORD cwa8 = static_cast<DWORD>(
        Dm()->ReadInt(mmhwnd, expression, 4));

    expression.Format(_T("[[%08x]+%08x]+%08x"),
                       bagJ, kPetTableOffset, petOffset + kPetStatusOffset2);
    const DWORD cwac = static_cast<DWORD>(
        Dm()->ReadInt(mmhwnd, expression, 4));

    expression.Format(_T("[[[[%08x]+64]+148]+10]+%05x"),
                       roleBase, static_cast<DWORD>(kPetOutStatusOffset));
    const DWORD rwa8 = static_cast<DWORD>(
        Dm()->ReadInt(mmhwnd, expression, 4));

    expression.Format(_T("[[[[%08x]+64]+148]+10]+%05x"),
                       roleBase,
                       static_cast<DWORD>(kPetOutStatusOffset + kPetStatusOffset3));
    const DWORD rwac = static_cast<DWORD>(
        Dm()->ReadInt(mmhwnd, expression, 4));

    if (cwa8 == rwa8 && cwac == rwac) {
        Mams[Cox1] = cwa8;
        Mams[Cox2] = cwac;
        return true;
    }
    return false;
}

int CTaskThread::Read_CWHapp(int xuh)
{
    constexpr DWORD kPetRecordCount = 7;
    constexpr DWORD kPetHappinessOffset = 92;

    if (!m_dm || !m_ready.load() || !m_wnd || !m_wnd->hWnd ||
        xuh < 0 || static_cast<DWORD>(xuh) >= kPetRecordCount) {
        return 0;
    }

    const DWORD bagJ = static_cast<DWORD>(jz[TaskMemoryState::BagJ]);
    if (bagJ < kMinimumUserAddress) return 0;

    CString expression;
    expression.Format(_T("[[%08x]+%08x]+%08x"),
                       bagJ, kPetTableOffset,
                       static_cast<DWORD>(xuh) * kPetRecordSize +
                           kPetHappinessOffset);
    return static_cast<int>(Dm()->ReadInt(mmhwnd, expression, 4));
}

int CTaskThread::Read_CWID(int xuh)
{
    constexpr DWORD kPetRecordCount = 7;
    constexpr DWORD kPetIdOffset = 16;

    if (!m_dm || !m_ready.load() || !m_wnd || !m_wnd->hWnd ||
        xuh < 0 || static_cast<DWORD>(xuh) >= kPetRecordCount) {
        return 0;
    }

    const DWORD bagJ = static_cast<DWORD>(jz[TaskMemoryState::BagJ]);
    if (bagJ < kMinimumUserAddress) return 0;

    CString expression;
    expression.Format(_T("[[%08x]+%08x]+%08x"),
                       bagJ, kPetTableOffset,
                       static_cast<DWORD>(xuh) * kPetRecordSize +
                           kPetIdOffset);
    return static_cast<int>(Dm()->ReadInt(mmhwnd, expression, 4));
}

int CTaskThread::Read_CWSkillID(int xuh, int skillIndex)
{
    if (!m_dm || !m_ready.load() || !m_wnd || !m_wnd->hWnd ||
        xuh < 0 || static_cast<DWORD>(xuh) >= 7 ||
        skillIndex < 0 || static_cast<DWORD>(skillIndex) >= kPetSkillSlotCount) {
        return 0;
    }

    const DWORD bagJ = static_cast<DWORD>(jz[TaskMemoryState::BagJ]);
    if (!IsPlausibleStatePointer(bagJ)) return 0;

    // Pet::GetSkillIDbyIndex uses the same chain:
    // BagJ -> +0x6711C -> record[xuh] + 0x11C -> skill[index].
    const DWORD bagManager = Read_Dw(bagJ);
    if (!IsPlausibleStatePointer(bagManager)) return 0;
    const DWORD petTable = Read_Dw(bagManager + kPetTableOffset);
    if (!IsPlausibleStatePointer(petTable)) return 0;

    const DWORD record = petTable + static_cast<DWORD>(xuh) * kPetRecordSize;
    const DWORD skillArray = Read_Dw(record + kPetSkillArrayOffset);
    if (!IsPlausibleStatePointer(skillArray)) return 0;

    const DWORD skillEntry = Read_Dw(
        skillArray + static_cast<DWORD>(skillIndex) * sizeof(DWORD));
    if (!IsPlausibleStatePointer(skillEntry)) return 0;

    // The array stores CActionItem_PetSkill entries.  Their first field is a
    // pointer to the skill DBC object, whose first field is the numeric ID.
    const DWORD skillObject = Read_Dw(skillEntry);
    if (!IsPlausibleStatePointer(skillObject)) return 0;
    return static_cast<int>(Read_Dw(skillObject));
}

bool CTaskThread::Read_CWHaveSkill(int xuh, int skillId)
{
    if (skillId <= 0) return false;
    for (int index = 0; index < static_cast<int>(kPetSkillSlotCount); ++index) {
        if (Read_CWSkillID(xuh, index) == skillId) return true;
    }
    return false;
}

int CTaskThread::Read_CWXueJi(int xuh)
{
    if (Read_CWHaveSkill(xuh, static_cast<int>(kPetGaoJiXueJiSkillId))) {
        return 2;
    }
    return Read_CWHaveSkill(xuh, static_cast<int>(kPetXueJiSkillId)) ? 1 : 0;
}

BagItem CTaskThread::Read_BagX(int zl)
{
    if (!m_dm || !m_ready.load()) return BagItem{};

    CString name;
    CString basePath = tsg[TaskMemoryState::workdir];
    if (basePath.IsEmpty()) {
        basePath = CString(std::filesystem::current_path().c_str());
    }

    CString iniPath = basePath + _T("/FanXiao.ini");
    if (!std::filesystem::exists(std::filesystem::path(iniPath.GetString()))) {
        iniPath = basePath + _T("/config/FanXiao.ini");
    }

    if (zl == 1) name = Dm()->ReadIni(_T("usewup"), _T("xue"), iniPath.GetString());
    if (zl == 2) name = Dm()->ReadIni(_T("usewup"), _T("lan"), iniPath.GetString());
    if (zl == 3) name = Dm()->ReadIni(_T("usewup"), _T("con"), iniPath.GetString());
    if (zl == 4) name = Dm()->ReadIni(_T("usewup"), _T("dwf"), iniPath.GetString());
    return Read_BagN3(name, 0, zl);
}

BagItem CTaskThread::Read_BagN3(CString Nam, int djg, int zl)
{
    BagItem& item = bag[balsy];
    item = BagItem{};
    item.xuh = static_cast<DWORD>(-1);
    item.bjxcz = true;

    if (!m_control || m_control->IsStopping() || !m_dm ||
        !m_ready.load() || !m_wnd || !m_wnd->hWnd) {
        return item;
    }

    std::regex txtRegex;
    try {
        txtRegex = std::regex(CStringToNearbyAnsi(Nam));
    } catch (const std::regex_error&) {
        DebugLog(_T("Read_BagN3: invalid item regex"));
        return item;
    }

    int bagmax = 0;
    if (djg == 0) bagmax = static_cast<int>(Mams[BagMax1]);
    if (djg == 1) bagmax = static_cast<int>(Mams[BagMax2]);
    if (djg == 2) bagmax = 20;
    if (bagmax <= 0 || bagmax > 200) return item;

    const DWORD bagJ = static_cast<DWORD>(jz[TaskMemoryState::BagJ]);
    if (bagJ < kMinimumUserAddress) return item;

    for (int i = 0; i < bagmax; ++i) {
        CString expression;
        expression.Format(
            _T("[[[[[%08x]+22C00]+%08x]+2c]+18]"),
            bagJ,
            static_cast<DWORD>(djg * 400 + i * 4));
        const CString itemName = Dm()->ReadString(mmhwnd, expression, 0, 0);
        if (!std::regex_match(CStringToNearbyAnsi(itemName), txtRegex)) {
            continue;
        }

        item.xuh = static_cast<DWORD>(i + djg * 100);
        item.csxuh.Format(_T("%02x"), item.xuh);
        item.bjxcz = false;

        UpdateMP(tsg[TaskMemoryState::shuxbg], 0x400);
        if (dwls >= kMinimumUserAddress) {
            const DWORD* itemIds = reinterpret_cast<const DWORD*>(
                static_cast<ULONG_PTR>(dwls));
            item.diz = itemIds[item.xuh];
        }

        dwls = static_cast<DWORD>(
            Dm()->ReadInt(mmhwnd, tsg[TaskMemoryState::shuxbg], 4));
        expression.Format(_T("[%08x]+14"),
                          dwls + static_cast<DWORD>(djg * 400 + i * 4));
        UpdateMP(expression, 0x100);
        if (dwls >= kMinimumUserAddress) {
            const BYTE* itemData = reinterpret_cast<const BYTE*>(
                static_cast<ULONG_PTR>(dwls));
            item.sj1 = *reinterpret_cast<const DWORD*>(itemData + 0);
            item.sj2 = *reinterpret_cast<const DWORD*>(itemData + 4);
            item.sj3 = *reinterpret_cast<const DWORD*>(itemData + 8);
        }

        if (zl != 3) {
            item.cow1 = 0;
            item.cow2 = 0;
        } else {
            item.cow1 = Mams[Cox1];
            item.cow2 = Mams[Cox2];
        }
        break;
    }

    return item;
}

int CTaskThread::wPet_Chu(int xuh, int cz)
{

	Dm()->WriteIntAddr(mmhwnd, jz[TaskMemoryState::Nbsq] + 750 + 16, 0, Mams[Cox1]);
	Dm()->WriteIntAddr(mmhwnd, jz[TaskMemoryState::Nbsq] + 750 + 23, 0, Mams[Cox2]);
	Dm()->WriteIntAddr(mmhwnd, jz[TaskMemoryState::Nbsq] + 750 + 30, 0, cz);    
	Dm()->WriteIntAddr(mmhwnd, jz[TaskMemoryState::Nbsq] + ZHUBC, 0, jz[TaskMemoryState::Nbsq] + 750);
    Dm()->WriteIntAddr(mmhwnd, jz[TaskMemoryState::Nbsq] + ZHUBB, 0, 1);
    return 0;
}

void CTaskThread::wUseBag2(int zl)
{
    if (zl == 1) bag[balsy] = bag[baxue];
    if (zl == 2) bag[balsy] = bag[balan];
    if (zl == 3) bag[balsy] = bag[bacow];
    if (bag[balsy].bjxcz) return;                 // no usable item

    // reference: when the write does not take effect, re-read the slot once and retry
    for (size_t i = 0; i < 2; ++i) {
        if (bag[balsy].bjxcz) {
            DebugLog(_T("wUseBag2: item unavailable, type=%d"), zl);
            return;
        }
        if (wUseBag2()) break;

        bag[balsy] = Read_BagX(zl);
        if (zl == 1) bag[baxue] = bag[balsy];
        if (zl == 2) bag[balan] = bag[balsy];
        if (zl == 3) bag[bacow] = bag[balsy];
    }
}

bool CTaskThread::wUseBag2()
{
	Dm()->WriteDataAddr(mmhwnd, jz[TaskMemoryState::Nbsq] + 800 + 9, bag[balsy].csxuh);
	Dm()->WriteIntAddr(mmhwnd, jz[TaskMemoryState::Nbsq] + 800 + 13, 0, bag[balsy].sj3);
	Dm()->WriteIntAddr(mmhwnd, jz[TaskMemoryState::Nbsq] + 800 + 20, 0, bag[balsy].sj1);
	Dm()->WriteIntAddr(mmhwnd, jz[TaskMemoryState::Nbsq] + 800 + 27, 0, bag[balsy].sj2);
	Mams[TaskMemoryState::dZJID] = Dm()->ReadInt(mmhwnd, tsg[TaskMemoryState::ZJID], 4);//每次使用物品更新
	Dm()->WriteIntAddr(mmhwnd, jz[TaskMemoryState::Nbsq] + 800 + 34, 0, Mams[TaskMemoryState::dZJID]);//打怪开始就更新 不换图不更新
	Dm()->WriteIntAddr(mmhwnd, jz[TaskMemoryState::Nbsq] + 800 + 62, 0, bag[balsy].cow1);
	Dm()->WriteIntAddr(mmhwnd, jz[TaskMemoryState::Nbsq] + 800 + 69, 0, bag[balsy].cow2);
	Dm()->WriteIntAddr(mmhwnd, jz[TaskMemoryState::Nbsq] + ZHUBC, 0, jz[TaskMemoryState::Nbsq] + 800);
    Dm()->WriteIntAddr(mmhwnd, jz[TaskMemoryState::Nbsq] + ZHUBB, 0, 1);
    return true;// use command issued (reference semantics)
}

bool CTaskThread::wUseBag2x(int xuh)
{
    DebugLog(_T("wUseBag2x: slot=%d"), xuh);
    if (xuh < 0 || static_cast<DWORD>(xuh + 2) > Mams[BagMax1]) return false;

    bag[balsy].xuh = static_cast<DWORD>(xuh);
    bag[balsy].csxuh.Format(_T("%02x"), bag[balsy].xuh);
    bag[balsy].bjxcz = false;

    // item data table: reference [BagJ]+22A9C -> base + 0x14 + index*4
    dwls = Read_Dw(tsg[TaskMemoryState::shuxbg]);
    CString expression;
    expression.Format(_T("[%08x]+14"),
                      dwls + static_cast<DWORD>(xuh) * 4);
    if (UpdateMP(expression, 0x100) && dwls >= kMinimumUserAddress) {
        const auto* itemData = reinterpret_cast<const BYTE*>(
            static_cast<ULONG_PTR>(dwls));
        bag[balsy].sj1 = *reinterpret_cast<const DWORD*>(itemData + 0);
        bag[balsy].sj2 = *reinterpret_cast<const DWORD*>(itemData + 4);
        bag[balsy].sj3 = *reinterpret_cast<const DWORD*>(itemData + 8);
        bag[balsy].cow1 = 0;
        bag[balsy].cow2 = 0;
    }

    wUseBag2();
    return true;
}

CString CTaskThread::mGet_Kjda(CString cTim)
{
    const std::wstring query = KjdaNormalize(std::wstring(
        cTim.GetString(), static_cast<size_t>(cTim.GetLength())));
    if (query.empty()) return CString();

    const std::vector<KjdaEntry>& entries = KjdaEntries();
    const KjdaEntry* bestEntry = nullptr;
    double bestSimilarity = 0.0;
    for (const KjdaEntry& entry : entries) {
        const double similarity = KjdaSimilarity(query, entry.question);
        if (similarity > bestSimilarity) {
            bestSimilarity = similarity;
            bestEntry = &entry;
            if (bestSimilarity >= 1.0) break;
        }
    }

    // The default acceptance threshold is 80 percent.  Returning an empty
    // CString keeps callers from accidentally using an unrelated answer.
    if (bestEntry == nullptr || bestSimilarity < 0.80) return CString();
    return CString(bestEntry->answer.c_str());
}

DWORD CTaskThread::CopyNearbyDescriptor(DWORD node, CString& expression) {
    expression = BuildNearbyDescriptorExpression(node);
    if (!m_dm || !m_ready.load() || !m_wnd || !m_wnd->hWnd || node == 0) {
        return 0;
    }

    const DWORD remoteDescriptor = Read_Dw(expression);
    if (remoteDescriptor == 0) return 0;

    return static_cast<DWORD>(m_dm->ReadDataAddrToBin(
        mmhwnd, static_cast<LONGLONG>(remoteDescriptor),
        static_cast<long>(kNearbyDescriptorCopyLength)));
}

float CTaskThread::NearbyDistance(const Point2f& from, const Point2f& to) {
    const float dx = to.x - from.x;
    const float dy = to.y - from.y;
    return std::sqrt(dx * dx + dy * dy);
}

void CTaskThread::NerbAr(DWORD Erc) {
    varrou.clear();
    if (!m_dm || !m_ready.load() || !m_wnd || !m_wnd->hWnd || Erc == 0) {
        return;
    }

    // The reference data is a binary tree.  Keep the original in-order
    // contract (node, right child, left child) but protect the worker from a
    // stale/corrupt pointer chain looping forever.
    std::unordered_set<DWORD> visited;
    std::function<void(DWORD)> visit = [&](DWORD node) {
        if (node < 0x10000 || visited.size() >= 4096 ||
            !visited.insert(node).second) {
            return;
        }

        const CString state = m_dm->ReadDataAddr(
            mmhwnd, static_cast<LONGLONG>(node + kNearbyTreeStatusOffset), 1);
        if (state.CompareNoCase(_T("00")) != 0) return;

        varrou.push_back(node + kNearbyTreeEntityOffset);
        visit(Read_Dw(node + kNearbyTreeLeftOffset));
        visit(Read_Dw(node + kNearbyTreeRightOffset));
    };

    visit(Erc);
}

int CTaskThread::NerbMonst2(DWORD Erc, regex txt_reg) {
    varrou.clear();
    NerbAr(Erc);
    vMonst.clear();
    if (varrou.empty()) return 0;

    Point2f playerPosition{};
    if (!ReadNearbyPlayerPosition(playerPosition)) {
        DebugLog(_T("nearby scan skipped: player position is unavailable"));
        return 0;
    }

    for (const DWORD node : varrou) {
        CString descriptorExpression;
        const DWORD descriptorCopy = CopyNearbyDescriptor(node, descriptorExpression);
        if (descriptorCopy == 0) continue;

        const CString name = NearbyAnsiBufferToCString(
            descriptorCopy, kNearbyNameOffset);
        const std::string nameAnsi = CStringToNearbyAnsi(name);
        if (name.IsEmpty() ||
            !std::regex_match(nameAnsi, txt_reg)) {
            continue;
        }

        float hp = 0.0f;
        if (!ReadNearbyLocalValue(descriptorCopy, kNearbyHpOffset, hp) || hp <= 0.0f) {
            continue;
        }

        if (!UpdateMP(node, kNearbyEntityCopyLength)) continue;
        const DWORD entityCopy = dwls;
        Point2f position{};
        if (!ReadNearbyLocalValue(entityCopy, kNearbyXOffset, position.x) ||
            !ReadNearbyLocalValue(entityCopy, kNearbyYOffset, position.y)) {
            continue;
        }

        const float distance = NearbyDistance(playerPosition, position);
        if (distance >= m_nearbyDistanceLimit) continue;

        int id = 0;
        (void)ReadNearbyLocalValue(entityCopy, kNearbyNameOffset, id);
        vMonst.push_back(MakeNearbyMonster(
            id, node, distance, descriptorExpression, name, position));
    }

    CString message;
    message.Format(_T("nearby regex scan: nodes=%lu matches=%lu"),
                   static_cast<unsigned long>(varrou.size()),
                   static_cast<unsigned long>(vMonst.size()));
    DebugLog(message);
    return static_cast<int>(vMonst.size());
}

int CTaskThread::NerbMonst3(DWORD Erc, int leix) {
    varrou.clear();
    NerbAr(Erc);
    vMonst.clear();
    if (varrou.empty()) return 0;

    Point2f origin{};
    if (leix) {
        origin = m_nearbyReferencePosition;
    } else if (!ReadNearbyPlayerPosition(origin)) {
        DebugLog(_T("nearby scan skipped: player position is unavailable"));
        return 0;
    }

    for (const DWORD node : varrou) {
        if (vMonst.size() >= 49) break;
        if (node < 4) continue;

        const DWORD typeId = Read_Dw(node - 4);
        if (typeId >= 0xFFFF) continue;

        const CString name = Dm()->ReadString(
            mmhwnd, BuildNearbyNameExpression(node).GetString(), 0, 0);
        if (name.IsEmpty() || !IsNearbyChineseText(name)) continue;

        if (!UpdateMP(node, kNearbyEntityCopyLength)) continue;
        const DWORD entityCopy = dwls;
        Point2f position{};
        if (!ReadNearbyLocalValue(entityCopy, kNearbyXOffset, position.x) ||
            !ReadNearbyLocalValue(entityCopy, kNearbyYOffset, position.y)) {
            continue;
        }

        const float distance = NearbyDistance(origin, position);
        if (distance >= m_nearbyDistanceLimit) continue;

        int id = 0;
        (void)ReadNearbyLocalValue(entityCopy, kNearbyNameOffset, id);
        vMonst.push_back(MakeNearbyMonster(
            id, node, distance, BuildNearbyNameExpression(node), name, position));
    }

    CString message;
    message.Format(_T("nearby type scan: nodes=%lu matches=%lu"),
                   static_cast<unsigned long>(varrou.size()),
                   static_cast<unsigned long>(vMonst.size()));
    DebugLog(message);
    return static_cast<int>(vMonst.size());
}

int CTaskThread::NerbMonst3(DWORD Erc, int leix, CString txt_reg) {
    varrou.clear();
    NerbAr(Erc);
    vMonst.clear();
    if (varrou.empty()) return 0;

    Point2f playerPosition{};

    if (!ReadNearbyPlayerPosition(playerPosition)) {
        DebugLog(_T("nearby scan skipped: player position is unavailable"));
        return 0;
    }

    for (const DWORD node : varrou) {
        if (vMonst.size() >= 49) break;
        const CString name = Dm()->ReadString(
            mmhwnd, BuildNearbyNameExpression(node).GetString(), 0, 0);
        if (name.IsEmpty() || name.Find(txt_reg) == -1) continue;

        if (!UpdateMP(node, kNearbyEntityCopyLength)) continue;
        const DWORD entityCopy = dwls;
        Point2f position{};
        if (!ReadNearbyLocalValue(entityCopy, kNearbyXOffset, position.x) ||
            !ReadNearbyLocalValue(entityCopy, kNearbyYOffset, position.y)) {
            continue;
        }

        const float distance = NearbyDistance(playerPosition, position);
        if (distance >= m_nearbyDistanceLimit) continue;

        int id = 0;
        (void)ReadNearbyLocalValue(entityCopy, kNearbyNameOffset, id);
        vMonst.push_back(MakeNearbyMonster(
            id, node, distance, BuildNearbyNameExpression(node), name, position));
    }

    CString message;
    message.Format(_T("nearby name scan: nodes=%lu matches=%lu"),
                   static_cast<unsigned long>(varrou.size()),
                   static_cast<unsigned long>(vMonst.size()));
    DebugLog(message);
    return static_cast<int>(vMonst.size());
}

int CTaskThread::NerbXian(DWORD Erc, CString txt_reg) {
    varrou.clear();
    NerbAr(Erc);
    vMonst.clear();
    if (varrou.empty()) return 0;

    Point2f playerPosition{};
    if (!ReadNearbyPlayerPosition(playerPosition)) {
        DebugLog(_T("trap scan skipped: player position is unavailable"));
        return 0;
    }

    for (const DWORD node : varrou) {
        const CString name = Dm()->ReadString(
            mmhwnd, BuildNearbyTrapNameExpression(node).GetString(), 0, 0);
        if (name.IsEmpty() || name.Find(txt_reg) == -1) continue;

        if (!UpdateMP(node, kNearbyEntityCopyLength)) continue;
        const DWORD entityCopy = dwls;
        Point2f position{};
        if (!ReadNearbyLocalValue(entityCopy, kNearbyTrapXOffset, position.x) ||
            !ReadNearbyLocalValue(entityCopy, kNearbyTrapYOffset, position.y)) {
            continue;
        }

        const float distance = NearbyDistance(playerPosition, position);
        if (distance >= m_nearbyDistanceLimit) continue;

        int id = 0;
        (void)ReadNearbyLocalValue(entityCopy, kNearbyNameOffset, id);
        vMonst.push_back(MakeNearbyMonster(
            id, node, distance, BuildNearbyTrapNameExpression(node), name, position));
    }

    CString message;
    message.Format(_T("nearby trap scan: nodes=%lu matches=%lu"),
                   static_cast<unsigned long>(varrou.size()),
                   static_cast<unsigned long>(vMonst.size()));
    DebugLog(message);
    return static_cast<int>(vMonst.size());
}

int CTaskThread::NerbMonst222(DWORD Erc, regex txt_reg) {
    varrou.clear();
    NerbAr(Erc);
    vMonst.clear();
    if (varrou.empty()) return 0;

    Point2f playerPosition{};
    if (!ReadNearbyPlayerPosition(playerPosition)) {
        DebugLog(_T("nearby fixed-point scan skipped: player position is unavailable"));
        return 0;
    }

    for (const DWORD node : varrou) {
        CString descriptorExpression;
        const DWORD descriptorCopy = CopyNearbyDescriptor(node, descriptorExpression);
        if (descriptorCopy == 0) continue;

        const CString name = NearbyAnsiBufferToCString(
            descriptorCopy, kNearbyNameOffset);
        const std::string nameAnsi = CStringToNearbyAnsi(name);
        if (name.IsEmpty() || !std::regex_match(nameAnsi, txt_reg)) continue;

        if (!UpdateMP(node, kNearbyEntityCopyLength)) continue;
        const DWORD entityCopy = dwls;
        Point2f position{};
        if (!ReadNearbyLocalValue(entityCopy, kNearbyXOffset, position.x) ||
            !ReadNearbyLocalValue(entityCopy, kNearbyYOffset, position.y)) {
            continue;
        }

        const float distance = NearbyDistance(playerPosition, position);
        if (distance >= m_nearbyDistanceLimit) continue;

        int id = 0;
        (void)ReadNearbyLocalValue(entityCopy, kNearbyNameOffset, id);
        vMonst.push_back(MakeNearbyMonster(
            id, node, distance, descriptorExpression, name, position));
    }

    CString message;
    message.Format(_T("nearby fixed-point scan: nodes=%lu matches=%lu"),
                   static_cast<unsigned long>(varrou.size()),
                   static_cast<unsigned long>(vMonst.size()));
    DebugLog(message);
    return static_cast<int>(vMonst.size());
}

void CTaskThread::wUseSk6(DWORD jiid, DWORD moid)
{
	Dm()->WriteIntAddr(mmhwnd, jz[TaskMemoryState::Nbsq] + 590 + 16, 0, jiid);
	Dm()->WriteIntAddr(mmhwnd, jz[TaskMemoryState::Nbsq] + 590 + 45, 0, moid);
	Dm()->WriteIntAddr(mmhwnd, jz[TaskMemoryState::Nbsq] + ZHUBC, 0, jz[TaskMemoryState::Nbsq] + 590);
    Dm()->WriteIntAddr(mmhwnd, jz[TaskMemoryState::Nbsq] + ZHUBB, 0, 1);
}

void CTaskThread::NearbyMonst5(DWORD Erc) {
    varrou.clear();
    NerbAr(Erc);
    aMonst = Monst{};
    if (varrou.empty()) return;

    Point2f playerPosition{};
    if (!ReadNearbyPlayerPosition(playerPosition)) {
        DebugLog(_T("nearby map scan skipped: player position is unavailable"));
        return;
    }

    static const LPCTSTR kMapNames[] = {
        _T("\u51F6\u5F92"), _T("\u6B79\u4EBA"), _T("\u523A\u5BA2"), _T("\u6076\u9738"),
        _T("\u5F3A\u76D7"), _T("\u6076\u8D3C"), _T("\u6076\u4EBA")
    };

    for (const DWORD node : varrou) {
        const CString title = Dm()->ReadString(
            mmhwnd, BuildNearbyTitleExpression(node).GetString(), 0, 0);
        if (title.GetLength() != 4) continue;

        bool matched = false;
        for (const LPCTSTR mapName : kMapNames) {
            if (title.Find(mapName) != -1) {
                matched = true;
                break;
            }
        }
        if (!matched || !UpdateMP(node, kNearbyEntityCopyLength)) continue;

        const DWORD entityCopy = dwls;
        Point2f position{};
        if (!ReadNearbyLocalValue(entityCopy, kNearbyXOffset, position.x) ||
            !ReadNearbyLocalValue(entityCopy, kNearbyYOffset, position.y)) {
            continue;
        }

        const float distance = NearbyDistance(playerPosition, position);
        if (distance >= 3.0f) continue;

        int id = 0;
        (void)ReadNearbyLocalValue(entityCopy, kNearbyNameOffset, id);
        aMonst = MakeNearbyMonster(
            id, node, distance, BuildNearbyDescriptorExpression(node), title, position);
        break;
    }
}

void CTaskThread::mLua_Dostr(CString csc)
{
    Dm()->WriteStringAddr(mmhwnd, jz[TaskMemoryState::Nbsq] + 0x1000, 0, csc);
    Dm()->WriteIntAddr(mmhwnd, jz[TaskMemoryState::Nbsq] + ZHUTC, 0, jz[TaskMemoryState::Nbsq] + 670);//标准
    Dm()->WriteIntAddr(mmhwnd, jz[TaskMemoryState::Nbsq] + ZHUTB, 0, 1);
}

void CTaskThread::wClickNpc(DWORD djg, DWORD gjg)
{
    CString tsls;
    const DWORD renW = static_cast<DWORD>(jz[TaskMemoryState::RenW]);
	tsls.Format(_T("[[%08x]+2C]+%08x"), renW, djg * 0x118 + 0x118);
	DWORD cwid = Dm()->ReadInt(mmhwnd, tsls, 4);
    Dm()->WriteIntAddr(mmhwnd, jz[TaskMemoryState::Nbsq] + 700 + 9, 0, cwid);
	tsls.Format(_T("[[%08x]+2C]"), renW);
	DWORD cwid2 = Dm()->ReadInt(mmhwnd, tsls, 4);
    Dm()->WriteIntAddr(mmhwnd, jz[TaskMemoryState::Nbsq] + 700 + 23, 0, cwid2);
	tsls.Format(_T("[[%08x]+2C]+%08x"), renW, djg * 0x118 + 0x10);
	DWORD cwid3 = Dm()->ReadInt(mmhwnd, tsls, 4);
    Dm()->WriteIntAddr(mmhwnd, jz[TaskMemoryState::Nbsq] + 700 + 16, 0, cwid3);
    Dm()->WriteIntAddr(mmhwnd, jz[TaskMemoryState::Nbsq] + ZHUBC, 0, jz[TaskMemoryState::Nbsq] + 700);
    Dm()->WriteIntAddr(mmhwnd, jz[TaskMemoryState::Nbsq] + ZHUBB, 0, 1);
}

bool CTaskThread::NerExist() {
    varrou.clear();
    const DWORD root = Read_Dw(tsg[TaskMemoryState::ERC]);
    NerbAr(root);
    return aMonst.Dibu != 0 &&
           std::find(varrou.begin(), varrou.end(), aMonst.Dibu) != varrou.end();
}

bool CTaskThread::NerExist(DWORD Mid) {
    varrou.clear();
    const DWORD root = Read_Dw(tsg[TaskMemoryState::ERC]);
    NerbAr(root);
    return Mid != 0 &&
           std::find(varrou.begin(), varrou.end(), Mid) != varrou.end();
}

DWORD CTaskThread::NerExist(CString cname) {
    varrou.clear();
    const DWORD root = Read_Dw(tsg[TaskMemoryState::ERC]);
    NerbAr(root);

    for (const DWORD node : varrou) {
        const CString name = Dm()->ReadString(
            mmhwnd, BuildNearbyNameExpression(node).GetString(), 0, 0);
        if (!name.IsEmpty() && name.Find(cname) != -1) return node;
    }
    return 0;
}

DWORD CTaskThread::FindNearbyObjectByName(const CString& name) {
    if (name.IsEmpty() || !m_dm || !m_ready.load() ||
        !m_wnd || !m_wnd->hWnd) {
        return 0;
    }

    const long targetHwnd = static_cast<long>(reinterpret_cast<INT_PTR>(m_wnd->hWnd));
    LONGLONG gameModule = m_dm->GetModuleBaseAddr(targetHwnd, _T("Game.exe"));
    if (gameModule == 0 && m_memory) gameModule = m_memory->gameModule;
    if (gameModule == 0) return 0;

    const DWORD game = static_cast<DWORD>(gameModule);
    const DWORD objectManager = Read_Dw(game + kObjectManagerGlobalRva);
    if (!IsPlausibleStatePointer(objectManager)) return 0;

    const auto objectFromEntitySlot = [&](DWORD entitySlot) -> DWORD {
        if (!IsPlausibleStatePointer(entitySlot)) return 0;
        const CString objectName = Dm()->ReadString(
            mmhwnd, BuildNearbyNameExpression(entitySlot).GetString(), 0, 0);
        if (objectName.Find(name) == -1) return 0;

        // Both object trees store the CObject_* pointer in node + 0x14.
        const DWORD object = Read_Dw(entitySlot);
        return IsPlausibleStatePointer(object) ? object : 0;
    };

    // First search the nearby-object tree (+0x58), whose root is at head+4.
    const DWORD nearbyHead = Read_Dw(objectManager + 0x58);
    if (IsPlausibleStatePointer(nearbyHead)) {
        const DWORD nearbyRoot = Read_Dw(nearbyHead + 0x04);
        if (IsPlausibleStatePointer(nearbyRoot) && nearbyRoot != nearbyHead) {
            NerbAr(nearbyRoot);
            for (const DWORD entitySlot : varrou) {
                const DWORD object = objectFromEntitySlot(entitySlot);
                if (object != 0) return object;
            }
        }
    }

    // Pets/summoners can be absent from the nearby tree while still present
    // in the handle-indexed object tree at +0x50.  That tree uses the same
    // node value field but its left/right links are +0/+8 respectively.
    const DWORD handleHead = Read_Dw(objectManager + kObjectManagerTreeOffset);
    if (!IsPlausibleStatePointer(handleHead)) return 0;
    const DWORD handleRoot = Read_Dw(handleHead + 0x04);
    if (!IsPlausibleStatePointer(handleRoot) || handleRoot == handleHead) {
        return 0;
    }

    std::vector<DWORD> pending;
    pending.push_back(handleRoot);
    std::unordered_set<DWORD> visited;
    visited.reserve(4096);
    while (!pending.empty() && visited.size() < 4096) {
        const DWORD node = pending.back();
        pending.pop_back();
        if (!IsPlausibleStatePointer(node) || node == handleHead ||
            !visited.insert(node).second) {
            continue;
        }
        if ((Read_Dw(node + 0x0D) & 0xFFu) != 0) continue;

        const DWORD object = objectFromEntitySlot(node + 0x14);
        if (object != 0) return object;

        const DWORD left = Read_Dw(node + 0x00);
        const DWORD right = Read_Dw(node + 0x08);
        if (left != 0) pending.push_back(left);
        if (right != 0) pending.push_back(right);
    }
    return 0;
}

bool CTaskThread::wHaveRide() {
    if (!m_dm || !m_ready.load() || !m_wnd || !m_wnd->hWnd) return false;

    const long targetHwnd = static_cast<long>(reinterpret_cast<INT_PTR>(m_wnd->hWnd));
    // zqpd is populated during game-memory initialization by dereferencing
    // [Game.exe + 0x007C6034], so it already holds the ride-list object.
    const DWORD object = static_cast<DWORD>(jz[TaskMemoryState::zqpd]);
    if (object == 0) return false;

    const LONGLONG record0 = static_cast<LONGLONG>(object) + kRideTableOffset;
    const WORD id = static_cast<WORD>(m_dm->ReadIntAddr(
        targetHwnd, record0, 2));
    const DWORD flag = static_cast<DWORD>(m_dm->ReadIntAddr(
        targetHwnd, record0 + 4, 4));
    return id > 0 && flag != 0;
}

void CTaskThread::wRideOn(int sx) {
    if (sx != 0 && sx != 1) return;
    if (!m_dm || !m_ready.load() || !m_wnd || !m_wnd->hWnd ||
        !m_control || m_control->IsStopping()) {
        return;
    }

    // Character-data offsets used by the reference implementation:
    // +0x4C is the movement speed (mounted is > 4.7), and +0x68C is the
    // generated/battle state which must be idle before changing mounts.
    constexpr DWORD kRideSpeedOffset = 0x4C;
    constexpr DWORD kRideGenerationOffset = 0x68C;

    const auto readCharacterData = [this]() -> DWORD {
        if (tsg[TaskMemoryState::Shuxtb].IsEmpty()) return 0;
        const DWORD characterData = Read_Dw(tsg[TaskMemoryState::Shuxtb]);
        return IsPlausibleStatePointer(characterData) ? characterData : 0;
    };
    const auto readRideSpeed = [&]() -> float {
        const DWORD characterData = readCharacterData();
        if (characterData == 0) return -1.0f;
        return Dm()->ReadFloatAddr(
            mmhwnd, static_cast<LONGLONG>(characterData) + kRideSpeedOffset);
    };
    const auto readGenerationState = [&]() -> DWORD {
        const DWORD characterData = readCharacterData();
        return characterData == 0
            ? 0
            : Read_Dw(characterData + kRideGenerationOffset);
    };

    // Match the original wait-for-idle guard, but keep it interruptible by
    // the framework stop controller.
    while (readGenerationState() == 1 && !m_control->IsStopping()) {
        SleepEx(1000);
    }
    if (m_control->IsStopping()) return;

    // Mounting requires an available, highlighted ride.  wHaveRide already
    // performs the live ride-table check for this build.
    if (sx == 1 && !wHaveRide()) {
        LogD(_T("坐骑操作：没有可用坐骑"));
        return;
    }

    float speed = readRideSpeed();
    for (int retry = 0; speed == -1.0f && retry < 20 &&
                            !m_control->IsStopping(); ++retry) {
        SleepEx(1000);
        speed = readRideSpeed();
    }
    if (speed == -1.0f || m_control->IsStopping()) return;

    LogD(_T("坐骑操作：%d   %.1f"), sx, speed);
    if (sx == 1) {
        if (speed > 4.7f) return;  // already mounted
        wUseSk6(0x15, static_cast<DWORD>(-1));
        for (int index = 0; index < 10 && !m_control->IsStopping(); ++index) {
            SleepEx(500);
            speed = readRideSpeed();
            if (speed > 4.1f) break;
        }
    } else {
        if (speed < 4.7f) return;  // already dismounted
        wUseSk6(0x15, static_cast<DWORD>(-1));
        for (int index = 0; index < 10 && !m_control->IsStopping(); ++index) {
            SleepEx(500);
            speed = readRideSpeed();
            if (speed >= 0.0f && speed < 4.1f) break;
        }

        // Dismounting during the mount-status transition can leave the
        // movement target stale. Re-issue a tiny offset from the live player
        // position, equivalent to the reference UpdateZB + wBenTuXuL2 path.
        if (!m_control->IsStopping() && Read_RWZT() == 5) {
            Point2f position{};
            if (ReadNearbyPlayerPosition(position)) {
                wBenTuXuL2(position.x + 0.1f, position.y);
            }
            SleepEx(500);
        }
    }

    if (!m_control->IsStopping()) SleepEx(1000);
}

DWORD CTaskThread::mRead_Buffer(DWORD xuh, CString* name,
                                CString* description, DWORD* stateObject) {
    if (name != nullptr) name->Empty();
    if (description != nullptr) description->Empty();
    if (stateObject != nullptr) *stateObject = 0;

    if (!m_dm || !m_ready.load() || !m_wnd || !m_wnd->hWnd) {
        return 0;
    }

    const long targetHwnd = static_cast<long>(reinterpret_cast<INT_PTR>(m_wnd->hWnd));
    LONGLONG gameModule = m_dm->GetModuleBaseAddr(targetHwnd, _T("Game.exe"));
    if (gameModule == 0 && m_memory) gameModule = m_memory->gameModule;
    if (gameModule == 0) return 0;

    const DWORD game = static_cast<DWORD>(gameModule);
    const DWORD bufferManager = Read_Dw(game + kStateBufferGlobalRva);
    return ReadStateBufferFromObject(
        m_dm.get(), targetHwnd, game, bufferManager, xuh,
        name, description, stateObject);
}

DWORD CTaskThread::mRead_BufferCount(DWORD characterObject) {
    if (!m_dm || !m_ready.load() || !m_wnd || !m_wnd->hWnd ||
        !IsPlausibleStatePointer(characterObject)) {
        return 0;
    }

    const DWORD count = Read_Dw(characterObject + kStateBufferCountOffset);
    return count <= kMaximumStateEntries ? count : 0;
}

DWORD CTaskThread::mRead_TargetBufferCount(DWORD targetObject) {
    if (!m_dm || !m_ready.load() || !m_wnd || !m_wnd->hWnd ||
        !IsPlausibleStatePointer(targetObject)) {
        return 0;
    }

    const DWORD treeHead = Read_Dw(
        targetObject + kTargetStateTreeOffset);
    if (!IsPlausibleStatePointer(treeHead)) return 0;

    const DWORD count = Read_Dw(targetObject + kTargetStateCountOffset);
    return count <= kMaximumStateEntries ? count : 0;
}

DWORD CTaskThread::mRead_BufferFromObject(
    DWORD characterObject, DWORD xuh, CString* name,
    CString* description, DWORD* stateObject) {
    if (name != nullptr) name->Empty();
    if (description != nullptr) description->Empty();
    if (stateObject != nullptr) *stateObject = 0;

    if (!m_dm || !m_ready.load() || !m_wnd || !m_wnd->hWnd ||
        !IsPlausibleStatePointer(characterObject)) {
        return 0;
    }

    const long targetHwnd = static_cast<long>(reinterpret_cast<INT_PTR>(m_wnd->hWnd));
    LONGLONG gameModule = m_dm->GetModuleBaseAddr(targetHwnd, _T("Game.exe"));
    if (gameModule == 0 && m_memory) gameModule = m_memory->gameModule;
    if (gameModule == 0) return 0;

    return ReadStateBufferFromObject(
        m_dm.get(), targetHwnd, static_cast<DWORD>(gameModule),
        characterObject, xuh, name, description, stateObject);
}

DWORD CTaskThread::mRead_TargetBufferFromObject(
    DWORD targetObject, DWORD xuh, CString* name,
    CString* description, DWORD* stateObject) {
    if (name != nullptr) name->Empty();
    if (description != nullptr) description->Empty();
    if (stateObject != nullptr) *stateObject = 0;

    if (!m_dm || !m_ready.load() || !m_wnd || !m_wnd->hWnd ||
        !IsPlausibleStatePointer(targetObject)) {
        return 0;
    }

    const long targetHwnd = static_cast<long>(reinterpret_cast<INT_PTR>(m_wnd->hWnd));
    LONGLONG gameModule = m_dm->GetModuleBaseAddr(targetHwnd, _T("Game.exe"));
    if (gameModule == 0 && m_memory) gameModule = m_memory->gameModule;
    if (gameModule == 0) return 0;

    return ReadTargetStateBufferFromObject(
        m_dm.get(), targetHwnd, static_cast<DWORD>(gameModule),
        targetObject, xuh, name, description, stateObject);
}

DWORD CTaskThread::mRead_TargetObject() {
    if (!m_dm || !m_ready.load() || !m_wnd || !m_wnd->hWnd) return 0;

    const long targetHwnd = static_cast<long>(reinterpret_cast<INT_PTR>(m_wnd->hWnd));
    LONGLONG gameModule = m_dm->GetModuleBaseAddr(targetHwnd, _T("Game.exe"));
    if (gameModule == 0 && m_memory) gameModule = m_memory->gameModule;
    if (gameModule == 0) return 0;

    const DWORD game = static_cast<DWORD>(gameModule);
    const DWORD objectManager = Read_Dw(game + kObjectManagerGlobalRva);
    if (!IsPlausibleStatePointer(objectManager)) return 0;

    // The selection handle is the canonical source used by the game's
    // resolver. Prefer resolving it from ObjectManager's tree; fall back to
    // the +0x48 cache when the handle/tree is being rebuilt during a scene
    // transition.
    const DWORD dataPool = Read_Dw(game + kStateBufferGlobalRva);
    if (IsPlausibleStatePointer(dataPool)) {
        const LONG selectionHandle = static_cast<LONG>(Read_Dw(
            dataPool + kSelectionHandleOffset));
        if (selectionHandle == -1) return 0;
        if (selectionHandle >= 0) {
            const DWORD resolved = FindObjectByHandle(
                m_dm.get(), targetHwnd, objectManager, selectionHandle);
            if (IsPlausibleStatePointer(resolved)) return resolved;
        }
    }

    const DWORD cachedTarget = Read_Dw(
        objectManager + kObjectManagerTargetOffset);
    return IsPlausibleStatePointer(cachedTarget) ? cachedTarget : 0;
}

DWORD CTaskThread::mRead_TargetBuffer(
    DWORD xuh, CString* name, CString* description, DWORD* stateObject) {
    if (name != nullptr) name->Empty();
    if (description != nullptr) description->Empty();
    if (stateObject != nullptr) *stateObject = 0;

    const DWORD targetObject = mRead_TargetObject();
    if (targetObject == 0) return 0;

    return mRead_TargetBufferFromObject(
        targetObject, xuh, name, description, stateObject);
}

DWORD CTaskThread::Read_RWZTC2(DWORD erc, std::vector<DWORD> vzt) {
    varrou.clear();
    if (!m_dm || !m_ready.load() || !m_wnd || !m_wnd->hWnd ||
        !m_memory || vzt.empty()) {
        return 0;
    }

    const long targetHwnd = static_cast<long>(
        reinterpret_cast<INT_PTR>(m_wnd->hWnd));
    DWORD statusObject = 0;
    if (erc == 0x1111) {
        // Self status object: [[ROLEB]+0x64]. ROLEB is the live role slot,
        // so dereference it once before following the +0x64 component.
        const DWORD roleSlot = static_cast<DWORD>(jz[TaskMemoryState::ROLEB]);
        if (IsPlausibleStatePointer(roleSlot)) {
            const DWORD roleObject = Read_Dw(roleSlot);
            if (IsPlausibleStatePointer(roleObject)) {
                statusObject = Read_Dw(roleObject + 0x64);
            }
        }
    } else {
        if (erc == static_cast<DWORD>(-1)) return 0;
        // Nearby entries pass the object-manager value slot; dereference it
        // once to obtain the target object consumed by the +0x130 tree.
        statusObject = Read_Dw(erc);
    }

    if (!IsPlausibleStatePointer(statusObject)) return 0;
    const DWORD treeHead = Read_Dw(statusObject + 0x130);
    const DWORD count = Read_Dw(statusObject + 0x134);
    if (!IsPlausibleStatePointer(treeHead) || count == 0 ||
        count > kMaximumStateEntries) {
        return 0;
    }

    CollectStatusTreeValues(m_dm.get(), targetHwnd, treeHead, count, varrou);
    for (const DWORD stateId : varrou) {
        if (std::find(vzt.begin(), vzt.end(), stateId) != vzt.end()) {
            return stateId;
        }
    }
    return 0;
}

DWORD CTaskThread::Read_RWZTC2(DWORD erc, DWORD zt) {
    varrou.clear();
    if (!m_dm || !m_ready.load() || !m_wnd || !m_wnd->hWnd ||
        !m_memory) {
        return 0;
    }

    const long targetHwnd = static_cast<long>(
        reinterpret_cast<INT_PTR>(m_wnd->hWnd));
    DWORD statusObject = 0;
    if (erc == 0x1111) {
        const DWORD roleSlot = static_cast<DWORD>(jz[TaskMemoryState::ROLEB]);
        if (IsPlausibleStatePointer(roleSlot)) {
            const DWORD roleObject = Read_Dw(roleSlot);
            if (IsPlausibleStatePointer(roleObject)) {
                statusObject = Read_Dw(roleObject + 0x64);
            }
        }
    } else {
        if (erc == static_cast<DWORD>(-1)) return 0;
        statusObject = Read_Dw(erc);
    }

    if (!IsPlausibleStatePointer(statusObject)) return 0;
    const DWORD treeHead = Read_Dw(statusObject + 0x130);
    const DWORD count = Read_Dw(statusObject + 0x134);
    if (!IsPlausibleStatePointer(treeHead) || count == 0 ||
        count > kMaximumStateEntries) {
        return 0;
    }

    const bool walked = CollectStatusTreeValues(
        m_dm.get(), targetHwnd, treeHead, count, varrou);
    if (!walked) return 0;
    return std::find(varrou.begin(), varrou.end(), zt) != varrou.end()
        ? zt
        : 0;
}

DWORD CTaskThread::Read_RWZTgs() {
    if (!m_dm || !m_ready.load() || !m_wnd || !m_wnd->hWnd) return 0;

    const DWORD roleSlot = static_cast<DWORD>(jz[TaskMemoryState::ROLEB]);
    if (!IsPlausibleStatePointer(roleSlot)) return 0;
    const DWORD roleObject = Read_Dw(roleSlot);
    if (!IsPlausibleStatePointer(roleObject)) return 0;

    const DWORD statusObject = Read_Dw(roleObject + 0x64);
    if (!IsPlausibleStatePointer(statusObject)) return 0;

    const DWORD count = Read_Dw(statusObject + 0x134);
    return count <= kMaximumStateEntries ? count : 0;
}

DWORD CTaskThread::Read_RWJY() {
    // Current build: experience is character-data + 0x29C0 (the reference
    // project used the older 0x29B8 offset).
    if (!m_dm || !m_ready.load() || !m_wnd || !m_wnd->hWnd ||
        tsg[TaskMemoryState::Shuxtb].IsEmpty()) {
        return 0;
    }

    const DWORD characterData = Read_Dw(tsg[TaskMemoryState::Shuxtb]);
    if (!IsPlausibleStatePointer(characterData)) return 0;
    return Read_Dw(characterData + 0x29C0);
}

int CTaskThread::Read_GENS() {
    if (!m_dm || !m_ready.load() || !m_wnd || !m_wnd->hWnd ||
        tsg[TaskMemoryState::Shuxtb].IsEmpty()) {
        return 0;
    }

    const DWORD characterData = Read_Dw(tsg[TaskMemoryState::Shuxtb]);
    if (!IsPlausibleStatePointer(characterData)) return 0;
    return static_cast<int>(Read_Dw(characterData + 0x68C));
}

int CTaskThread::Read_MoZTT(DWORD gwdz) {
    if (!m_dm || !m_ready.load() || !m_wnd || !m_wnd->hWnd ||
        gwdz == 0 || gwdz == static_cast<DWORD>(-1)) {
        return 0;
    }

    // Nearby records pass the value slot (the same +0x14 slot retained in
    // Monst::Dibu). Dereference it first, then read the monster state at
    // object +0x14C as in the reference expression "[gwdz]+14C".
    const DWORD object = Read_Dw(gwdz);
    if (!IsPlausibleStatePointer(object)) return 0;
    return static_cast<int>(Read_Dw(object + 0x14C));
}

int CTaskThread::Read_ATID() {
    if (!m_dm || !m_ready.load() || !m_wnd || !m_wnd->hWnd ||
        tsg[TaskMemoryState::GJID].IsEmpty()) {
        return 0;
    }
    return static_cast<int>(Read_Dw(tsg[TaskMemoryState::GJID]));
}

int CTaskThread::Read_ATID(DWORD gwdz) {
    if (!m_dm || !m_ready.load() || !m_wnd || !m_wnd->hWnd ||
        gwdz == 0 || gwdz == static_cast<DWORD>(-1)) {
        return 0;
    }

    const DWORD object = Read_Dw(gwdz);
    if (!IsPlausibleStatePointer(object)) return 0;
    return static_cast<int>(Read_Dw(object + 0x214));
}

int CTaskThread::Read_DTID() {
    if (!m_dm || !m_ready.load() || !m_wnd || !m_wnd->hWnd) return 0;

    // MYDITUP resolves to the live map object.  Its current map ID is stored
    // at +0x50, matching the reference expression "[MYDITUP]+50".
    const DWORD mapObject = static_cast<DWORD>(jz[TaskMemoryState::MyDiTu]);
    if (!IsPlausibleStatePointer(mapObject)) return 0;
    return static_cast<int>(Read_Dw(mapObject + 0x50));
}

int CTaskThread::Read_RWZT(bool qzjc) {
    // Match the reference implementation: normal reads are throttled to one
    // remote-memory query every 500 ms, while qzjc forces an immediate read.
    // The cached value/timestamp live in TaskMemoryState so all worker
    // threads for a window observe the same state, just like the original
    // Mams[drwzt]/Mams[dztsj] slots.
    const DWORD now = ::GetTickCount();
    if (!qzjc && now - Mams[dztsj] < 500) {
        return static_cast<int>(Mams[drwzt]);
    }

    Mams[dztsj] = now;
    if (!m_dm || !m_ready.load() || !m_wnd || !m_wnd->hWnd ||
        tsg[TaskMemoryState::m_Dqzt].IsEmpty()) {
        Mams[drwzt] = 0;
        return 0;
    }

    Mams[drwzt] = Read_Dw(tsg[TaskMemoryState::m_Dqzt]);
    return static_cast<int>(Mams[drwzt]);
}

bool CTaskThread::mCanAttack(DWORD zjdz, DWORD Mid)
{
    if (!m_dm || !m_ready.load() || !m_wnd || !m_wnd->hWnd ||
        !m_memory || zjdz == 0 || Mid == 0) {
        return false;
    }

    const long targetHwnd = static_cast<long>(reinterpret_cast<INT_PTR>(m_wnd->hWnd));
    // The module cache can be empty when Fub_Test runs immediately after the
    // worker is ready, and it can become stale after the game logs in again.
    // Read the live base for this query instead of treating the cache as a
    // prerequisite for the relation calculation.
    const LONGLONG gameModule = m_dm->GetModuleBaseAddr(targetHwnd, _T("Game.exe"));
    if (gameModule == 0) return false;

    const auto readDword = [this, targetHwnd](LONGLONG address) {
        return static_cast<DWORD>(m_dm->ReadIntAddr(targetHwnd, address, 4));
    };
    const auto readWord = [this, targetHwnd](LONGLONG address) {
        // dmsoft's ReadIntAddr type=2 is returned as the low byte by this
        // wrapper for these fields (FFFF was observed as 00FF). Read the
        // 32-bit value and keep its low 16 bits instead.
        return static_cast<WORD>(m_dm->ReadIntAddr(targetHwnd, address, 4));
    };

    // 00547C30 builds its relation descriptors from this chain:
    // object + 0x148 -> character component; component + 0x10 -> character data.
    constexpr LONGLONG kCharacterComponentOffset = 0x148;
    constexpr LONGLONG kCharacterDataOffset = 0x10;
    constexpr LONGLONG kRelationTypeOffset = 0x44;
    constexpr LONGLONG kCampOffset = 0xA0;
    constexpr LONGLONG kRelationDisabledOffset = 0xD4;
    constexpr LONGLONG kRelationOverrideOffset = 0x2B20;

    const DWORD selfComponent = readDword(
        static_cast<LONGLONG>(zjdz) + kCharacterComponentOffset);
    const DWORD targetComponent = readDword(
        static_cast<LONGLONG>(Mid) + kCharacterComponentOffset);
    if (selfComponent == 0 || targetComponent == 0 || zjdz == Mid) return false;

    const DWORD selfData = readDword(
        static_cast<LONGLONG>(selfComponent) + kCharacterDataOffset);
    const DWORD targetData = readDword(
        static_cast<LONGLONG>(targetComponent) + kCharacterDataOffset);
    if (selfData == 0 || targetData == 0) return false;

    // sub_8AFF40 rejects the local relation descriptor when this field is 1.
    if (readDword(static_cast<LONGLONG>(selfData) + kRelationDisabledOffset) == 1) {
        return false;
    }

    const WORD selfCamp = readWord(static_cast<LONGLONG>(selfData) + kCampOffset);
    const WORD targetCamp = readWord(static_cast<LONGLONG>(targetData) + kCampOffset);

    // When both camp values are present, 008AFEA0 is only this direct test.
    if (selfCamp != 0xFFFF && targetCamp != 0xFFFF) {
        return selfCamp != targetCamp &&
               selfCamp != 0 && targetCamp != 0 &&
               selfCamp != 1 && targetCamp != 1;
    }

    const WORD selfType = readWord(static_cast<LONGLONG>(selfData) + kRelationTypeOffset);
    const WORD targetType = readWord(static_cast<LONGLONG>(targetData) + kRelationTypeOffset);
    if (selfType > 0x3F || targetType > 0x3F) return false;

    // 008B3FF0 reads a 64x64 inline relation matrix. For the captured sample:
    // selfType=9, targetType=0x1B, matrix=-160000 and self override=0.
    constexpr LONGLONG kRelationMatrixRva = 0x007D32F0;
    constexpr LONGLONG kRelationRowStride = 0x110;
    constexpr LONGLONG kRelationRowDataOffset = 0x0C;
    constexpr LONG kAttackableThreshold = -160000;

    const LONGLONG matrix = gameModule + kRelationMatrixRva;
    if (readDword(matrix) == 0 || readDword(matrix + 4) == 0) return false;

    const LONGLONG row = matrix + 8 +
        static_cast<LONGLONG>(selfType) * kRelationRowStride;
    if (readDword(row) == 0) return false;

    const LONG matrixRelation = static_cast<LONG>(readDword(
        row + kRelationRowDataOffset + static_cast<LONGLONG>(targetType) * 4));
    const LONG objectOverride = static_cast<LONG>(readDword(
        static_cast<LONGLONG>(selfData) + kRelationOverrideOffset +
        static_cast<LONGLONG>(targetType) * 4));
    const LONGLONG relation = static_cast<LONGLONG>(matrixRelation) + objectOverride;
    return relation <= kAttackableThreshold;
}
