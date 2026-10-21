#include "host_locale.hpp"
#include "localization_config.hpp"

#include <Windows.h>

#include <fstream>
#include <iterator>
#include <string>
#include <unordered_map>

namespace erui::host {
namespace {

constexpr std::uintmax_t kMaximumLocaleBytes = 64 * 1024;

std::string_view trim(std::string_view value) noexcept {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1);
    }
    while (!value.empty() &&
        (value.back() == ' ' || value.back() == '\t' || value.back() == '\r')) {
        value.remove_suffix(1);
    }
    return value;
}

bool utf8_to_utf16(std::string_view input, std::wstring& output) {
    if (input.empty() || input.size() > static_cast<std::size_t>(INT_MAX) ||
        input.find('\0') != std::string_view::npos) {
        return false;
    }
    const int count = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
        static_cast<int>(input.size()), nullptr, 0);
    if (count <= 0) return false;
    std::wstring converted(static_cast<std::size_t>(count), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
            static_cast<int>(input.size()), converted.data(), count) != count ||
        converted.find(L'\0') != std::wstring::npos) {
        return false;
    }
    output = std::move(converted);
    return true;
}

// Built-in host text for Elden Ring's Steam languages, so no locales folder
// is needed. A locales\<identifier>.ini next to the DLL still overrides any
// key. nullptr keeps the English default.
struct BuiltinLocale {
    const char* identifier;
    const wchar_t* menu_help;
    const wchar_t* previous_label;
    const wchar_t* previous_help;
    const wchar_t* next_label;
    const wchar_t* next_help;
};

constexpr BuiltinLocale kBuiltinLocales[] = {
    {"arabic", nullptr, L"الصفحة السابقة", L"العودة إلى صفحة الإعدادات السابقة.",
        L"الصفحة التالية", L"فتح صفحة الإعدادات التالية."},
    {"brazilian", nullptr, L"Página anterior", L"Voltar à página de configurações anterior.",
        L"Próxima página", L"Abrir a próxima página de configurações."},
    {"french", nullptr, L"Page précédente", L"Revenir à la page de réglages précédente.",
        L"Page suivante", L"Ouvrir la page de réglages suivante."},
    {"german", nullptr, L"Vorherige Seite", L"Zur vorherigen Einstellungsseite zurückkehren.",
        L"Nächste Seite", L"Die nächste Einstellungsseite öffnen."},
    {"italian", nullptr, L"Pagina precedente", L"Torna alla pagina delle impostazioni precedente.",
        L"Pagina successiva", L"Apri la pagina delle impostazioni successiva."},
    {"japanese", nullptr, L"前のページ", L"前の設定ページに戻ります。",
        L"次のページ", L"次の設定ページを開きます。"},
    {"koreana", nullptr, L"이전 페이지", L"이전 설정 페이지로 돌아갑니다.",
        L"다음 페이지", L"다음 설정 페이지를 엽니다."},
    {"latam", L"Ajustes proporcionados por los mods instalados.", L"Página anterior",
        L"Volver a la página de configuración anterior.", L"Página siguiente",
        L"Abrir la página de configuración siguiente."},
    {"polish", nullptr, L"Poprzednia strona", L"Wróć do poprzedniej strony ustawień.",
        L"Następna strona", L"Otwórz następną stronę ustawień."},
    {"russian", nullptr, L"Предыдущая страница", L"Вернуться на предыдущую страницу настроек.",
        L"Следующая страница", L"Открыть следующую страницу настроек."},
    {"schinese", nullptr, L"上一页", L"返回上一设置页。", L"下一页", L"打开下一设置页。"},
    {"spanish", L"Ajustes proporcionados por los mods instalados.", L"Página anterior",
        L"Volver a la página de ajustes anterior.", L"Página siguiente",
        L"Abrir la página de ajustes siguiente."},
    {"tchinese", nullptr, L"上一頁", L"返回上一個設定頁面。", L"下一頁", L"開啟下一個設定頁面。"},
    {"thai", nullptr, L"หน้าก่อนหน้า", L"กลับไปยังหน้าการตั้งค่าก่อนหน้า",
        L"หน้าถัดไป", L"เปิดหน้าการตั้งค่าถัดไป"},
    {"vietnamese", nullptr, L"Trang trước", L"Quay lại trang cài đặt trước đó.",
        L"Trang sau", L"Mở trang cài đặt tiếp theo."},
};

void apply_builtin(std::string_view identifier, erui::MenuLocalization& text) {
    for (const BuiltinLocale& locale : kBuiltinLocales) {
        if (identifier != locale.identifier) continue;
        const auto set = [](const wchar_t* value, std::wstring& destination) {
            if (value) destination = value;
        };
        set(locale.menu_help, text.menu_help);
        set(locale.previous_label, text.previous_label);
        set(locale.previous_help, text.previous_help);
        set(locale.next_label, text.next_label);
        set(locale.next_help, text.next_help);
        return;
    }
}

} // namespace

HostLocale load_host_locale(
    const std::filesystem::path& dll_directory,
    std::string_view language_identifier) noexcept {
    HostLocale result{};
    if (!is_safe_locale_identifier(language_identifier)) return result;
    try {
        apply_builtin(language_identifier, result.pagination);
    } catch (...) {
        result = HostLocale{};
    }
    try {
        const std::filesystem::path path = dll_directory / L"locales" /
            (std::filesystem::path(language_identifier) += L".ini");
        std::error_code error{};
        const std::uintmax_t size = std::filesystem::file_size(path, error);
        if (error || size == 0 || size > kMaximumLocaleBytes) return result;

        std::ifstream stream(path, std::ios::binary);
        if (!stream) return result;
        std::string bytes{
            std::istreambuf_iterator<char>(stream),
            std::istreambuf_iterator<char>()};
        if (bytes.size() != static_cast<std::size_t>(size)) return result;
        if (bytes.size() >= 3 &&
            static_cast<unsigned char>(bytes[0]) == 0xEF &&
            static_cast<unsigned char>(bytes[1]) == 0xBB &&
            static_cast<unsigned char>(bytes[2]) == 0xBF) {
            bytes.erase(0, 3);
        }

        std::unordered_map<std::string, std::string_view> values{};
        std::string section{};
        std::size_t cursor = 0;
        while (cursor <= bytes.size()) {
            const std::size_t end = bytes.find('\n', cursor);
            std::string_view line(bytes.data() + cursor,
                (end == std::string::npos ? bytes.size() : end) - cursor);
            line = trim(line);
            if (!line.empty() && line.front() != ';' && line.front() != '#') {
                if (line.size() >= 2 && line.front() == '[' && line.back() == ']') {
                    section.assign(trim(line.substr(1, line.size() - 2)));
                } else if (const std::size_t equals = line.find('=');
                           equals != std::string_view::npos) {
                    const std::string_view key = trim(line.substr(0, equals));
                    const std::string_view value = trim(line.substr(equals + 1));
                    if (!section.empty() && !key.empty() && !value.empty()) {
                        values.insert_or_assign(section + "." + std::string(key), value);
                    }
                }
            }
            if (end == std::string::npos) break;
            cursor = end + 1;
        }

        const auto apply = [&](const char* key, std::wstring& destination) {
            const auto found = values.find(key);
            if (found != values.end()) {
                (void)utf8_to_utf16(found->second, destination);
            }
        };
        apply("Menu.Title", result.pagination.menu_title);
        apply("Menu.Help", result.pagination.menu_help);
        apply("Pagination.PreviousLabel", result.pagination.previous_label);
        apply("Pagination.PreviousHelp", result.pagination.previous_help);
        apply("Pagination.NextLabel", result.pagination.next_label);
        apply("Pagination.NextHelp", result.pagination.next_help);
        result.loaded = true;
    } catch (...) {
        HostLocale fallback{};
        try {
            apply_builtin(language_identifier, fallback.pagination);
        } catch (...) {
            return HostLocale{};
        }
        return fallback;
    }
    return result;
}

} // namespace erui::host
