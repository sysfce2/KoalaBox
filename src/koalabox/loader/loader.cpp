#include "loader.hpp"

#include "koalabox/hook/hook.hpp"
#include "koalabox/logger/logger.hpp"
#include "koalabox/util/util.hpp"
#include "koalabox/win_util/win_util.hpp"

namespace koalabox::loader {

    [[maybe_unused]]
    Path get_module_dir(HMODULE& handle) {
        auto file_name = win_util::get_module_file_name(handle);

        auto module_path = Path(file_name);

        return module_path.parent_path();
    }

    [[maybe_unused]]
    hook::FunctionPointer get_original_function(
        bool is_hook_mode,
        HMODULE library,
        const String& function_name
    ) {
        if (is_hook_mode) {
            if (not hook::address_book.contains(function_name)) {
                util::panic(__func__, "Address book does not contain function: {}", function_name);
            }

            return hook::address_book[function_name];
        } else {
            return (hook::FunctionPointer) win_util::get_proc_address(
                library,
                function_name.c_str()
            );
        }
    }

    /**
     * Key is un-mangled name, value is mangled name
     */
    [[maybe_unused]]
    Map <String, String> get_undecorated_function_map(HMODULE library) {
        // Adapted from: https://github.com/mborne/dll2def/blob/master/dll2def.cpp

        auto exported_functions = Map<String, String>();

        auto dos_header = reinterpret_cast<PIMAGE_DOS_HEADER>(library);

        if (dos_header->e_magic != IMAGE_DOS_SIGNATURE) {
            util::panic(__func__, "e_magic  != IMAGE_DOS_SIGNATURE");
        }

        auto header = reinterpret_cast<PIMAGE_NT_HEADERS>((BYTE*) library + dos_header->e_lfanew );

        if (header->Signature != IMAGE_NT_SIGNATURE) {
            util::panic(__func__, "header->Signature != IMAGE_NT_SIGNATURE");
        }

        if (header->OptionalHeader.NumberOfRvaAndSizes <= 0) {
            util::panic(__func__, "header->OptionalHeader.NumberOfRvaAndSizes <= 0");
        }

        auto virtual_address = header->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
        auto exports = reinterpret_cast<PIMAGE_EXPORT_DIRECTORY>((BYTE*) library +
                                                                 virtual_address );
        PVOID names = (BYTE*) library + exports->AddressOfNames;

        // Iterate over the names and add them to the vector
        for (unsigned int i = 0; i < exports->NumberOfNames; i++) {
            String exported_name = (char*) library + ((DWORD*) names)[i];

            static const std::regex expression(R"((?:^_)?(\w+)(?:@\d+$)?)");

            String undecorated_function = exported_name; // fallback value

            std::smatch matches;
            if (std::regex_match(exported_name, matches, expression)) {
                if (matches.size() == 2) {
                    undecorated_function = matches[1];
                } else {
                    logger::warn("Exported function regex size != 2: {}", exported_name);
                }
            } else {
                logger::warn("Exported function regex failed: {}", exported_name);
            }

            exported_functions.insert({ undecorated_function, exported_name });
        }

        return exported_functions;
    }

    [[maybe_unused]]
    String get_undecorated_function(HMODULE library, const String& function_name) {
        if (util::is_64_bit()) {
            return function_name;
        } else {
            static auto undecorated_function_map = get_undecorated_function_map(library);

            return undecorated_function_map[function_name];
        }
    }

    [[maybe_unused]]
    HMODULE load_original_library(
        const Path& self_directory,
        const String& orig_library_name
    ) {
        const auto original_module_path = self_directory / (orig_library_name + "_o.dll");

        const auto original_module = win_util::load_library(original_module_path);

        logger::info("📚 Loaded original library from: '{}'", original_module_path.string());

        return original_module;
    }

}
