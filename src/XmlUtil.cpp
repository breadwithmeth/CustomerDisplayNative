#include "stdafx.h"
#include "XmlUtil.h"

namespace xmlutil {

namespace {

std::wstring ToLower(std::wstring s)
{
    for (auto& c : s)
        c = (wchar_t)::towlower(c);
    return s;
}

// Регистронезависимый поиск подстроки
size_t FindI(const std::wstring& hay, const std::wstring& needle, size_t from = 0)
{
    const std::wstring h = ToLower(hay);
    const std::wstring n = ToLower(needle);
    if (n.empty() || from > h.size())
        return std::wstring::npos;
    return h.find(n, from);
}

std::wstring Trim(const std::wstring& s)
{
    size_t b = 0, e = s.size();
    while (b < e && iswspace(s[b])) ++b;
    while (e > b && iswspace(s[e - 1])) --e;
    return s.substr(b, e - b);
}

// Разворачивает XML-сущности (&lt; &amp; &#NNN; &#xNNNN; и т.п.)
std::wstring DecodeEntities(const std::wstring& s)
{
    std::wstring out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != L'&') { out += s[i]; continue; }
        size_t semi = s.find(L';', i + 1);
        if (semi == std::wstring::npos || semi - i > 10) { out += s[i]; continue; }
        std::wstring ent = s.substr(i + 1, semi - i - 1);
        if (ent == L"lt") out += L'<';
        else if (ent == L"gt") out += L'>';
        else if (ent == L"amp") out += L'&';
        else if (ent == L"quot") out += L'"';
        else if (ent == L"apos") out += L'\'';
        else if (ent == L"nbsp") out += L' ';
        else if (!ent.empty() && (ent[0] == L'#')) {
            // &#1234; или &#x1F600;
            unsigned long code = ULONG_MAX;
            const wchar_t* num = ent.c_str() + 1;
            if (*num == L'x' || *num == L'X')
                code = ::wcstoul(num + 1, nullptr, 16);
            else
                code = ::wcstoul(num, nullptr, 10);
            if (code != ULONG_MAX && code <= 0x10FFFF)
                out += (wchar_t)code;
        }
        else { out += s.substr(i, semi - i + 1); }
        i = semi;
    }
    return out;
}

// Внутренний текст первого вхождения тега <tag ...>text</tag>
bool GetInnerText(const std::wstring& xml, const std::wstring& tag, std::wstring& out)
{
    size_t pos = 0;
    while (true) {
        size_t lt = FindI(xml, L"<" + tag, pos);
        if (lt == std::wstring::npos)
            return false;
        // убедимся, что это именно тег (далее пробел, '>' или '/')
        size_t nameEnd = lt + 1 + tag.size();
        if (nameEnd >= xml.size() || (xml[nameEnd] != L'>' && xml[nameEnd] != L'/' && !iswspace(xml[nameEnd]))) {
            pos = lt + 1;
            continue;
        }
        size_t gt = xml.find(L'>', lt);
        if (gt == std::wstring::npos)
            return false;
        if (xml[gt - 1] == L'/') { // самозакрытый <tag/>
            out.clear();
            return true;
        }
        size_t close = FindI(xml, L"</" + tag, gt);
        if (close == std::wstring::npos)
            return false;
        out = xml.substr(gt + 1, close - gt - 1);
        return true;
    }
}

} // anonymous namespace

std::map<std::wstring, std::wstring> ParseParameters(const std::wstring& xml)
{
    std::map<std::wstring, std::wstring> result;
    if (xml.empty())
        return result;

    size_t pos = 0;
    while (true) {
        size_t lt = FindI(xml, L"<Parameter", pos);
        if (lt == std::wstring::npos) {
            // дополнительно принимаем русское написание
            lt = FindI(xml, L"<Параметр", pos);
            if (lt == std::wstring::npos)
                break;
        }
        size_t gt = xml.find(L'>', lt);
        if (gt == std::wstring::npos)
            break;
        std::wstring tagText = xml.substr(lt, gt - lt + 1);

        std::wstring name = GetTagAttr(tagText, L"Parameter", L"Name");
        if (name.empty())
            name = GetTagAttr(tagText, L"Параметр", L"Имя");
        if (name.empty()) {
            pos = gt + 1;
            continue;
        }
        std::wstring value = GetTagAttr(tagText, L"Parameter", L"Value");
        if (value.empty())
            value = GetTagAttr(tagText, L"Параметр", L"Значение");
        // Значение может быть и внутренним текстом: <Parameter Name="X">V</Parameter>
        if (value.empty()) {
            std::wstring inner;
            if (GetInnerText(xml, L"Parameter", inner))
                value = inner;
        }
        result[DecodeEntities(name)] = DecodeEntities(Trim(value));
        pos = gt + 1;
    }
    return result;
}

std::vector<std::wstring> ParseDisplayLines(const std::wstring& data)
{
    std::vector<std::wstring> lines;
    if (Trim(data).empty())
        return lines;

    // Пытаемся распознать XML со строками
    static const wchar_t* kOpenTags[]  = { L"<String",  L"<Line",  L"<Строка",  L"<Строки" };
    static const wchar_t* kCloseTags[] = { L"</String>", L"</Line>", L"</Строка>", L"</Строки>" };

    size_t pos = 0;
    bool foundAnyTag = false;
    for (int attempt = 0; attempt < 4; ++attempt) {
        const std::wstring openTag = kOpenTags[attempt];
        const std::wstring closeTag = kCloseTags[attempt];
        std::vector<std::wstring> parsed;
        pos = 0;
        while (true) {
            size_t lt = FindI(data, openTag, pos);
            if (lt == std::wstring::npos)
                break;
            foundAnyTag = true;
            size_t nameEnd = lt + openTag.size();
            if (nameEnd >= data.size() || (data[nameEnd] != L'>' && data[nameEnd] != L'/' && !iswspace(data[nameEnd]))) {
                pos = lt + 1;
                continue;
            }
            size_t gt = data.find(L'>', lt);
            if (gt == std::wstring::npos)
                break;
            std::wstring openText = data.substr(lt, gt - lt + 1);
            // атрибут Value / Значение / Text / Текст
            std::wstring val = GetTagAttr(openText, openTag.substr(1), L"Value");
            if (val.empty()) val = GetTagAttr(openText, openTag.substr(1), L"Значение");
            if (val.empty()) val = GetTagAttr(openText, openTag.substr(1), L"Text");
            if (val.empty()) val = GetTagAttr(openText, openTag.substr(1), L"Текст");
            if (val.empty() && data[gt - 1] != L'/') {
                size_t close = FindI(data, closeTag, gt);
                if (close == std::wstring::npos)
                    break;
                val = data.substr(gt + 1, close - gt - 1);
                pos = close + closeTag.size();
            }
            else {
                pos = gt + 1;
            }
            // внутри строки может быть перенос — оставляем как есть, но тримим края
            std::wstring decoded = DecodeEntities(val);
            parsed.push_back(Trim(decoded));
        }
        if (!parsed.empty()) {
            lines = parsed;
            break;
        }
    }

    // Fallback: обычный текст с переносами строк (или XML не дал строк)
    if (lines.empty()) {
        if (foundAnyTag) {
            // XML есть, но теги строк не распознаны — выведем очищенный текст
            std::wstring text = data;
            // грубое снятие тегов
            size_t p;
            while ((p = text.find(L'<')) != std::wstring::npos) {
                size_t q = text.find(L'>', p);
                if (q == std::wstring::npos) break;
                text.erase(p, q - p + 1);
            }
            text = DecodeEntities(text);
            // разбиение по строкам
            std::wstring cur;
            for (wchar_t c : text) {
                if (c == L'\n') { lines.push_back(Trim(cur)); cur.clear(); }
                else if (c != L'\r') cur += c;
            }
            lines.push_back(Trim(cur));
        }
        else {
            std::wstring cur;
            for (wchar_t c : data) {
                if (c == L'\n') { lines.push_back(Trim(cur)); cur.clear(); }
                else if (c != L'\r') cur += c;
            }
            lines.push_back(Trim(cur));
        }
    }

    // убираем пустые хвостовые строки
    while (!lines.empty() && Trim(lines.back()).empty())
        lines.pop_back();
    return lines;
}

std::wstring GetTagAttr(const std::wstring& xml, const std::wstring& tag, const std::wstring& name)
{
    // находим <tag
    size_t lt = FindI(xml, L"<" + tag);
    if (lt == std::wstring::npos)
        return std::wstring();
    size_t gt = xml.find(L'>', lt);
    if (gt == std::wstring::npos)
        return std::wstring();
    std::wstring head = xml.substr(lt, gt - lt);

    // ищем name = "value" или name='value'
    size_t p = 0;
    while (true) {
        p = FindI(head, name, p);
        if (p == std::wstring::npos)
            return std::wstring();
        size_t after = p + name.size();
        while (after < head.size() && iswspace(head[after])) ++after;
        if (after >= head.size() || head[after] != L'=') { p = after; continue; }
        ++after;
        while (after < head.size() && iswspace(head[after])) ++after;
        if (after >= head.size() || (head[after] != L'"' && head[after] != L'\'')) { p = after; continue; }
        wchar_t quote = head[after];
        size_t end = head.find(quote, after + 1);
        if (end == std::wstring::npos)
            return std::wstring();
        return DecodeEntities(head.substr(after + 1, end - after - 1));
    }
}

std::wstring EscapeAttr(const std::wstring& s)
{
    std::wstring out;
    out.reserve(s.size() + 8);
    for (wchar_t c : s) {
        switch (c) {
        case L'&': out += L"&amp;"; break;
        case L'<': out += L"&lt;"; break;
        case L'>': out += L"&gt;"; break;
        case L'"': out += L"&quot;"; break;
        case L'\'': out += L"&apos;"; break;
        default: out += c;
        }
    }
    return out;
}

std::wstring EscapeText(const std::wstring& s)
{
    std::wstring out;
    out.reserve(s.size() + 8);
    for (wchar_t c : s) {
        switch (c) {
        case L'&': out += L"&amp;"; break;
        case L'<': out += L"&lt;"; break;
        case L'>': out += L"&gt;"; break;
        default: out += c;
        }
    }
    return out;
}

COLORREF ParseColor(const std::wstring& hex, COLORREF fallback)
{
    // Принимаем "RRGGBB", "#RRGGBB", "0xRRGGBB"
    std::wstring s = Trim(hex);
    if (!s.empty() && s[0] == L'#') s = s.substr(1);
    if (s.size() > 2 && s[0] == L'0' && (s[1] == L'x' || s[1] == L'X')) s = s.substr(2);
    if (s.size() != 6)
        return fallback;
    wchar_t buf[7] = { 0 };
    memcpy(buf, s.c_str(), 6 * sizeof(wchar_t));
    wchar_t* endp = nullptr;
    unsigned long v = ::wcstoul(buf, &endp, 16);
    if (endp != buf + 6)
        return fallback;
    return RGB((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
}

} // namespace xmlutil
