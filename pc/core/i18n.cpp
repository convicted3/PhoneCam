#include "i18n.h"

#include <windows.h>
#include <shlobj.h>

#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>

namespace phonecam {
namespace {

struct Entry { const char* ru; const char* en; const char* uk; };
const Entry kTable[] = {
#include "i18n_table.inc"
};

std::atomic<int> g_lang{-1};
std::mutex g_mu;

const std::unordered_map<std::string, const Entry*>& Index() {
    static const std::unordered_map<std::string, const Entry*> idx = [] {
        std::unordered_map<std::string, const Entry*> m;
        for (const Entry& e : kTable) m.emplace(e.ru, &e);
        return m;
    }();
    return idx;
}

std::string U8(const wchar_t* w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}
std::wstring W(const char* s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], n);
    return w;
}

}  // namespace

Lang LangFromCode(const char* code) {
    if (!code) return Lang::En;
    std::string c(code);
    for (auto& ch : c) ch = (char)tolower((unsigned char)ch);
    if (c.rfind("ru", 0) == 0 || c.rfind("be", 0) == 0 || c.rfind("kk", 0) == 0) return Lang::Ru;
    if (c.rfind("uk", 0) == 0) return Lang::Uk;
    return Lang::En;
}

Lang DetectLang() {
    wchar_t appdata[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appdata))) {
        std::wstring ini = std::wstring(appdata) + L"\\PhoneCam\\phonecam.ini";
        wchar_t v[16] = L"";
        GetPrivateProfileStringW(L"phonecam", L"lang", L"auto", v, 16, ini.c_str());
        std::wstring s(v);
        if (s == L"ru") return Lang::Ru;
        if (s == L"en") return Lang::En;
        if (s == L"uk") return Lang::Uk;
    }
    switch (PRIMARYLANGID(GetUserDefaultUILanguage())) {
    case LANG_RUSSIAN: case LANG_BELARUSIAN: case LANG_KAZAK: return Lang::Ru;
    case LANG_UKRAINIAN: return Lang::Uk;
    default: return Lang::En;
    }
}

Lang CurrentLang() {
    int l = g_lang;
    if (l < 0) {
        l = (int)DetectLang();
        int expected = -1;
        g_lang.compare_exchange_strong(expected, l);
        l = g_lang;
    }
    return (Lang)l;
}

void SetLang(Lang l) { g_lang = (int)l; }

}  // namespace phonecam

using namespace phonecam;

const char* TR(const char* ru) {
    Lang l = CurrentLang();
    if (l == Lang::Ru || !ru) return ru;
    auto& idx = Index();
    auto it = idx.find(ru);
    if (it == idx.end()) return ru;
    const char* t = l == Lang::En ? it->second->en : it->second->uk;
    return t ? t : ru;
}

const wchar_t* TRW(const wchar_t* ru) {
    Lang l = CurrentLang();
    if (l == Lang::Ru || !ru) return ru;
    std::lock_guard<std::mutex> lk(g_mu);
    static std::map<std::pair<const wchar_t*, int>, std::wstring> cache;
    auto key = std::make_pair(ru, (int)l);
    auto it = cache.find(key);
    if (it == cache.end()) {
        std::string k = U8(ru);
        const char* t = TR(k.c_str());
        it = cache.emplace(key, t == k.c_str() ? std::wstring(ru) : W(t)).first;
    }
    return it->second.c_str();
}
