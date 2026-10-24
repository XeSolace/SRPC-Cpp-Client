#include "srpc/client.hpp"

#include "protocol.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace srpc {
namespace {

namespace fs = std::filesystem;

struct ManifestEntry {
    fs::path local_path;
    std::string remote_path;
    std::uint64_t size = 0;
    bool is_directory = false;
};

struct TransferManifest {
    std::vector<ManifestEntry> entries;
    std::uint64_t total_size = 0;
    std::size_t file_count = 0;
};

char ascii_lower(char value) {
    if (value >= 'A' && value <= 'Z') {
        return static_cast<char>(value - 'A' + 'a');
    }
    return value;
}

std::string ascii_case_key(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), ascii_lower);
    return result;
}

bool is_windows_reserved_name(std::string_view name) {
    const auto dot = name.find('.');
    const auto stem = ascii_case_key(name.substr(0, dot));
    if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul" ||
        stem == "clock$" || stem == "conin$" || stem == "conout$") {
        return true;
    }
    return stem.size() == 4 &&
        (stem.starts_with("com") || stem.starts_with("lpt")) &&
        stem[3] >= '1' && stem[3] <= '9';
}

void validate_xbox_component(std::string_view name) {
    if (name.empty() || name == "." || name == "..") {
        throw ProtocolError("Invalid Xbox path component: " + std::string(name));
    }
    for (const unsigned char character : name) {
        if (character < 0x20 || character == 0x7F || character == '\\' ||
            character == '/' || character == ':' || character == '"') {
            throw ProtocolError(
                "Xbox path component contains a reserved character: " +
                std::string(name));
        }
    }
}

void validate_windows_component(std::string_view name) {
    validate_xbox_component(name);
    if (name.back() == ' ' || name.back() == '.') {
        throw ProtocolError(
            "Windows path components cannot end with a space or period: " +
            std::string(name));
    }
    if (name.find_first_of("<>|?*") != std::string_view::npos ||
        is_windows_reserved_name(name)) {
        throw ProtocolError(
            "Xbox entry cannot be represented safely as a Windows filename: " +
            std::string(name));
    }
}

fs::path local_component_from_remote(std::string_view name) {
    validate_windows_component(name);
    std::u8string encoded;
    encoded.reserve(name.size());
    for (const unsigned char character : name) {
        encoded.push_back(static_cast<char8_t>(character));
    }
    const fs::path component(encoded);
    if (component.empty() || component.has_root_path() ||
        component.filename() != component) {
        throw ProtocolError(
            "Xbox entry is not a single local filename component: " +
            std::string(name));
    }
    return component;
}

std::string remote_component_from_local(const fs::path& path) {
    const auto encoded = path.filename().u8string();
    std::string result;
    result.reserve(encoded.size());
    for (const auto character : encoded) {
        result.push_back(static_cast<char>(character));
    }
    validate_windows_component(result);
    return result;
}

void validate_remote_path(std::string_view path, std::string_view description) {
    if (path.empty()) {
        throw ProtocolError(std::string(description) + " cannot be empty");
    }
    static_cast<void>(detail::quote_xbdm_argument(path, description));
    constexpr std::size_t command_overhead_reserve = 64;
    if (path.size() > detail::max_command_length - command_overhead_reserve) {
        throw ProtocolError(
            std::string(description) + " is too long for XBDM file commands");
    }
}

std::string normalized_remote_root(
    std::string_view path,
    std::string_view description) {
    validate_remote_path(path, description);
    std::string result(path);
    std::replace(result.begin(), result.end(), '/', '\\');
    return result;
}

std::string join_remote_path(
    std::string_view parent,
    std::string_view component) {
    validate_xbox_component(component);
    std::string result(parent);
    if (result.empty() || result.back() != '\\') {
        result.push_back('\\');
    }
    result.append(component);
    validate_remote_path(result, "Joined remote path");
    return result;
}

bool local_path_exists(const fs::path& path) {
    std::error_code error;
    const bool exists = fs::exists(path, error);
    if (error) {
        throw fs::filesystem_error("Could not inspect local path", path, error);
    }
    return exists;
}

bool is_local_reparse_point(const fs::path& path) {
    std::error_code status_error;
    const auto link_status = fs::symlink_status(path, status_error);
    if (!status_error && fs::is_symlink(link_status)) {
        return true;
    }

    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const auto error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            return false;
        }
        throw fs::filesystem_error(
            "Could not inspect local path attributes",
            path,
            std::error_code(static_cast<int>(error), std::system_category()));
    }
    return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

void reject_nested_reparse_points(
    const fs::path& selected_root,
    const fs::path& nested_path) {
    std::error_code error;
    const auto root = fs::absolute(selected_root, error).lexically_normal();
    if (error) {
        throw fs::filesystem_error(
            "Could not resolve selected local root", selected_root, error);
    }
    const auto target = fs::absolute(nested_path, error).lexically_normal();
    if (error) {
        throw fs::filesystem_error(
            "Could not resolve nested local path", nested_path, error);
    }
    const auto relative = target.lexically_relative(root);
    if (relative.empty() && target != root) {
        throw ProtocolError("Local transfer path is outside the selected root");
    }

    auto current = root;
    for (const auto& component : relative) {
        if (component == "..") {
            throw ProtocolError("Local transfer path escapes the selected root");
        }
        if (component == ".") {
            continue;
        }
        current /= component;
        if (is_local_reparse_point(current)) {
            throw ProtocolError(
                "Refusing to traverse local reparse point: " +
                current.string());
        }
    }
}

bool local_path_is_directory(const fs::path& path) {
    std::error_code error;
    const bool result = fs::is_directory(path, error);
    if (error) {
        throw fs::filesystem_error("Could not inspect local directory", path, error);
    }
    return result;
}

void ensure_local_directory(
    const fs::path& path,
    bool may_create,
    bool allow_selected_root_reparse = false) {
    if (local_path_exists(path)) {
        if (!allow_selected_root_reparse && is_local_reparse_point(path)) {
            throw ProtocolError(
                "Refusing to use a local reparse point as a transfer directory: " +
                path.string());
        }
        if (!local_path_is_directory(path)) {
            throw fs::filesystem_error(
                "Local directory path is occupied by a non-directory",
                path,
                std::make_error_code(std::errc::not_a_directory));
        }
        return;
    }
    if (!may_create) {
        throw fs::filesystem_error(
            "Local destination root does not exist",
            path,
            std::make_error_code(std::errc::no_such_file_or_directory));
    }

    std::error_code error;
    static_cast<void>(fs::create_directories(path, error));
    if (error) {
        throw fs::filesystem_error("Could not create local directory", path, error);
    }
    if (is_local_reparse_point(path)) {
        throw ProtocolError(
            "Created local transfer directory unexpectedly became a reparse point");
    }
}

void validate_file_size(
    TransferManifest& manifest,
    std::uint64_t size,
    const DirectoryTransferOptions& options,
    std::string_view description) {
    if (size > std::numeric_limits<std::uint32_t>::max()) {
        throw ProtocolError(
            std::string(description) + " exceeds XBDM's 4 GiB file limit");
    }
    if (size > options.files.maximum_file_size) {
        throw ProtocolError(
            std::string(description) + " exceeds the configured per-file limit");
    }
    if (size > std::numeric_limits<std::uint64_t>::max() - manifest.total_size) {
        throw ProtocolError("Directory manifest byte total overflow");
    }
    const auto next_total = manifest.total_size + size;
    if (options.maximum_total_size && next_total > *options.maximum_total_size) {
        throw ProtocolError("Directory manifest exceeds the configured total-size limit");
    }
    manifest.total_size = next_total;
}

void validate_directory_transfer_options(
    const DirectoryTransferOptions& options) {
    constexpr std::size_t maximum_chunk_size = 16 * 1024 * 1024;
    if (options.files.chunk_size == 0 ||
        options.files.chunk_size > maximum_chunk_size) {
        throw ProtocolError(
            "Directory file-transfer chunk size must be between 1 byte and 16 MiB");
    }
    switch (options.files.existing_file) {
    case ExistingFilePolicy::fail:
    case ExistingFilePolicy::overwrite:
    case ExistingFilePolicy::skip:
        break;
    default:
        throw ProtocolError("Invalid existing-file policy");
    }
}

void append_manifest_entry(
    TransferManifest& manifest,
    ManifestEntry entry,
    const DirectoryTransferOptions& options) {
    if (manifest.entries.size() >= options.maximum_entries) {
        throw ProtocolError("Directory manifest exceeds the configured entry limit");
    }
    if (!entry.is_directory) {
        validate_file_size(manifest, entry.size, options, "Manifest file");
        if (manifest.file_count == std::numeric_limits<std::size_t>::max()) {
            throw ProtocolError("Directory manifest file-count overflow");
        }
        ++manifest.file_count;
    }
    manifest.entries.push_back(std::move(entry));
}

bool report_scanning_progress(
    const DirectoryTransferOptions& options,
    const TransferManifest& manifest,
    const fs::path& local_path,
    std::string_view remote_path,
    std::uint64_t file_size) {
    if (!options.files.progress) {
        return false;
    }
    TransferProgress progress;
    progress.phase = TransferPhase::scanning;
    progress.local_path = local_path;
    progress.remote_path = std::string(remote_path);
    progress.file_bytes_total = file_size;
    progress.overall_bytes_transferred = 0;
    progress.file_index = manifest.file_count;
    return options.files.progress(progress) == TransferControl::cancel;
}

bool scan_remote_directory(
    Client& client,
    std::string_view remote_directory,
    const fs::path& local_directory,
    std::size_t depth,
    const DirectoryTransferOptions& options,
    TransferManifest& manifest) {
    auto children = client.directory_contents(remote_directory);
    std::sort(
        children.begin(),
        children.end(),
        [](const DirectoryEntry& left, const DirectoryEntry& right) {
            const auto left_key = ascii_case_key(left.name);
            const auto right_key = ascii_case_key(right.name);
            return left_key == right_key ? left.name < right.name : left_key < right_key;
        });

    std::set<std::string> collision_keys;
    for (const auto& child : children) {
        if (depth >= options.maximum_depth) {
            throw ProtocolError("Remote directory exceeds the configured recursion depth");
        }
        const auto local_component = local_component_from_remote(child.name);
        const auto collision_key = ascii_case_key(child.name);
        if (!collision_keys.insert(collision_key).second) {
            throw ProtocolError(
                "Remote directory contains names that collide on Windows: " +
                child.name);
        }

        const auto remote_path = join_remote_path(remote_directory, child.name);
        const auto local_path = local_directory / local_component;
        ManifestEntry entry;
        entry.local_path = local_path;
        entry.remote_path = remote_path;
        entry.size = child.size;
        entry.is_directory = child.is_directory;
        append_manifest_entry(manifest, std::move(entry), options);
        if (report_scanning_progress(
                options,
                manifest,
                local_path,
                remote_path,
                child.is_directory ? 0 : child.size)) {
            return false;
        }
        if (child.is_directory && !scan_remote_directory(
                client,
                remote_path,
                local_path,
                depth + 1,
                options,
                manifest)) {
            return false;
        }
    }
    return true;
}

std::vector<fs::directory_entry> sorted_local_children(const fs::path& directory) {
    std::vector<fs::directory_entry> children;
    std::error_code error;
    fs::directory_iterator iterator(directory, fs::directory_options::none, error);
    if (error) {
        throw fs::filesystem_error("Could not enumerate local directory", directory, error);
    }
    const fs::directory_iterator end;
    while (iterator != end) {
        children.push_back(*iterator);
        iterator.increment(error);
        if (error) {
            throw fs::filesystem_error(
                "Could not continue enumerating local directory", directory, error);
        }
    }
    std::sort(
        children.begin(),
        children.end(),
        [](const fs::directory_entry& left, const fs::directory_entry& right) {
            return left.path().filename().native() < right.path().filename().native();
        });
    return children;
}

bool scan_local_directory(
    const fs::path& local_directory,
    std::string_view remote_directory,
    std::size_t depth,
    const DirectoryTransferOptions& options,
    TransferManifest& manifest) {
    const auto children = sorted_local_children(local_directory);
    std::set<std::string> collision_keys;
    for (const auto& child : children) {
        if (depth >= options.maximum_depth) {
            throw ProtocolError("Local directory exceeds the configured recursion depth");
        }
        if (is_local_reparse_point(child.path())) {
            throw ProtocolError(
                "Refusing to traverse nested local reparse point: " +
                child.path().string());
        }

        const auto component = remote_component_from_local(child.path());
        const auto collision_key = ascii_case_key(component);
        if (!collision_keys.insert(collision_key).second) {
            throw ProtocolError(
                "Local directory contains names that collide on Xbox: " + component);
        }
        const auto remote_path = join_remote_path(remote_directory, component);

        std::error_code status_error;
        const auto status = child.symlink_status(status_error);
        if (status_error) {
            throw fs::filesystem_error(
                "Could not inspect local manifest entry", child.path(), status_error);
        }

        ManifestEntry entry;
        entry.local_path = child.path();
        entry.remote_path = remote_path;
        if (fs::is_directory(status)) {
            entry.is_directory = true;
            append_manifest_entry(manifest, std::move(entry), options);
            if (report_scanning_progress(
                    options, manifest, child.path(), remote_path, 0)) {
                return false;
            }
            if (!scan_local_directory(
                    child.path(),
                    remote_path,
                    depth + 1,
                    options,
                    manifest)) {
                return false;
            }
            continue;
        }
        if (!fs::is_regular_file(status)) {
            throw ProtocolError(
                "Local manifest contains a non-regular file: " +
                child.path().string());
        }

        std::error_code size_error;
        entry.size = child.file_size(size_error);
        if (size_error) {
            throw fs::filesystem_error(
                "Could not determine local file size", child.path(), size_error);
        }
        append_manifest_entry(manifest, std::move(entry), options);
        if (report_scanning_progress(
                options,
                manifest,
                child.path(),
                remote_path,
                manifest.entries.back().size)) {
            return false;
        }
    }
    return true;
}

void validate_download_destinations(
    const fs::path& local_root,
    const TransferManifest& manifest,
    const DirectoryTransferOptions& options) {
    if (local_path_exists(local_root)) {
        if (!local_path_is_directory(local_root)) {
            throw fs::filesystem_error(
                "Local destination root is not a directory",
                local_root,
                std::make_error_code(std::errc::not_a_directory));
        }
    } else if (!options.create_destination_root) {
        throw fs::filesystem_error(
            "Local destination root does not exist",
            local_root,
            std::make_error_code(std::errc::no_such_file_or_directory));
    }

    for (const auto& entry : manifest.entries) {
        reject_nested_reparse_points(local_root, entry.local_path);
        if (!local_path_exists(entry.local_path)) {
            continue;
        }
        const bool is_directory = local_path_is_directory(entry.local_path);
        if (entry.is_directory != is_directory) {
            throw fs::filesystem_error(
                "Local destination has the wrong file type",
                entry.local_path,
                std::make_error_code(std::errc::file_exists));
        }
        if (!entry.is_directory &&
            options.files.existing_file == ExistingFilePolicy::fail) {
            throw fs::filesystem_error(
                "Local destination file already exists",
                entry.local_path,
                std::make_error_code(std::errc::file_exists));
        }
    }
}

void ensure_remote_directory(
    Client& client,
    std::string_view remote_path,
    bool may_create) {
    if (!may_create) {
        if (!client.is_directory(remote_path)) {
            throw ProtocolError(
                "Remote destination root is not an existing directory: " +
                std::string(remote_path));
        }
        return;
    }

    try {
        client.create_directory(remote_path);
    } catch (const CommandError& error) {
        if (error.status_code() != 410) {
            throw;
        }
        if (!client.is_directory(remote_path)) {
            throw ProtocolError(
                "Remote directory path is occupied by a non-directory: " +
                std::string(remote_path));
        }
    }
}

FileTransferOptions aggregate_file_options(
    const DirectoryTransferOptions& options,
    const ManifestEntry& entry,
    const TransferManifest& manifest,
    const TransferResult& completed,
    std::size_t file_index) {
    auto result = options.files;
    if (options.maximum_total_size) {
        if (completed.bytes_transferred > *options.maximum_total_size) {
            throw ProtocolError(
                "Completed transfer bytes exceed the configured total-size limit");
        }
        result.maximum_file_size = std::min(
            result.maximum_file_size,
            *options.maximum_total_size - completed.bytes_transferred);
    }

    const auto callback = options.files.progress;
    if (!callback) {
        result.progress = {};
        return result;
    }

    const auto* options_pointer = &options;
    const auto* manifest_pointer = &manifest;
    const auto* completed_pointer = &completed;
    const auto local_path = entry.local_path;
    const auto remote_path = entry.remote_path;
    const auto entry_size = entry.size;
    result.progress = [
        callback,
        file_index,
        options_pointer,
        manifest_pointer,
        completed_pointer,
        local_path,
        remote_path,
        entry_size](const TransferProgress& progress) {
        if (progress.file_bytes_transferred >
            std::numeric_limits<std::uint64_t>::max() -
                completed_pointer->bytes_transferred) {
            throw ProtocolError("Aggregate transfer progress byte-count overflow");
        }
        if (options_pointer->maximum_total_size &&
            progress.phase == TransferPhase::transferring &&
            progress.file_bytes_total >
                *options_pointer->maximum_total_size -
                    completed_pointer->bytes_transferred) {
            throw ProtocolError(
                "Actual directory transfer exceeds the configured total-size limit");
        }

        auto aggregate = progress;
        aggregate.local_path = local_path;
        aggregate.remote_path = remote_path;
        if (aggregate.phase == TransferPhase::file_skipped &&
            aggregate.file_bytes_total == 0) {
            aggregate.file_bytes_total = entry_size;
        }
        aggregate.overall_bytes_transferred =
            completed_pointer->bytes_transferred + progress.file_bytes_transferred;
        aggregate.overall_bytes_total = manifest_pointer->total_size;
        aggregate.file_index = file_index;
        aggregate.file_count = manifest_pointer->file_count;
        return callback(aggregate);
    };
    return result;
}

void add_transfer_result(TransferResult& aggregate, const TransferResult& file) {
    if (file.files_completed >
            std::numeric_limits<std::size_t>::max() - aggregate.files_completed ||
        file.files_skipped >
            std::numeric_limits<std::size_t>::max() - aggregate.files_skipped ||
        file.bytes_transferred >
            std::numeric_limits<std::uint64_t>::max() - aggregate.bytes_transferred) {
        throw ProtocolError("Aggregate transfer result overflow");
    }
    aggregate.files_completed += file.files_completed;
    aggregate.files_skipped += file.files_skipped;
    aggregate.bytes_transferred += file.bytes_transferred;
    aggregate.cancelled = file.cancelled;
}

} // namespace

TransferResult Client::download_directory_to(
    std::string_view remote_root,
    const fs::path& local_root,
    DirectoryTransferOptions options) {
    if (local_root.empty()) {
        throw ProtocolError("Local destination root cannot be empty");
    }
    validate_directory_transfer_options(options);
    const auto normalized_remote =
        normalized_remote_root(remote_root, "Remote source root");

    TransferManifest manifest;
    if (report_scanning_progress(
            options, manifest, local_root, normalized_remote, 0) ||
        !scan_remote_directory(
            *this,
            normalized_remote,
            local_root,
            0,
            options,
            manifest)) {
        TransferResult result;
        result.cancelled = true;
        return result;
    }

    validate_download_destinations(local_root, manifest, options);
    ensure_local_directory(
        local_root, options.create_destination_root, true);

    TransferResult result;
    std::size_t file_index = 0;
    for (const auto& entry : manifest.entries) {
        if (entry.is_directory) {
            ensure_local_directory(entry.local_path, true);
            continue;
        }

        ++file_index;
        reject_nested_reparse_points(local_root, entry.local_path.parent_path());
        auto file_options = aggregate_file_options(
            options, entry, manifest, result, file_index);
        const auto file_result =
            download_file_to(entry.remote_path, entry.local_path, std::move(file_options));
        add_transfer_result(result, file_result);
        if (result.cancelled) {
            return result;
        }
    }
    return result;
}

TransferResult Client::upload_directory_from(
    const fs::path& local_root,
    std::string_view remote_root,
    DirectoryTransferOptions options) {
    if (local_root.empty()) {
        throw ProtocolError("Local source root cannot be empty");
    }
    validate_directory_transfer_options(options);
    if (!local_path_exists(local_root) || !local_path_is_directory(local_root)) {
        throw fs::filesystem_error(
            "Local upload root is not a directory",
            local_root,
            std::make_error_code(std::errc::not_a_directory));
    }
    const auto normalized_remote =
        normalized_remote_root(remote_root, "Remote destination root");

    TransferManifest manifest;
    if (report_scanning_progress(
            options, manifest, local_root, normalized_remote, 0) ||
        !scan_local_directory(
            local_root,
            normalized_remote,
            0,
            options,
            manifest)) {
        TransferResult result;
        result.cancelled = true;
        return result;
    }

    ensure_remote_directory(*this, normalized_remote, options.create_destination_root);

    TransferResult result;
    std::size_t file_index = 0;
    for (const auto& entry : manifest.entries) {
        if (entry.is_directory) {
            ensure_remote_directory(*this, entry.remote_path, true);
            continue;
        }

        ++file_index;
        reject_nested_reparse_points(local_root, entry.local_path);
        auto file_options = aggregate_file_options(
            options, entry, manifest, result, file_index);
        const auto file_result =
            upload_file_from(entry.local_path, entry.remote_path, std::move(file_options));
        add_transfer_result(result, file_result);
        if (result.cancelled) {
            return result;
        }
    }
    return result;
}

} // namespace srpc
