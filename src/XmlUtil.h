// XmlUtil.h — минимальные средства разбора и сборки XML для драйвера
// дисплея покупателя (формат строк обмена — по стандарту 1С
// «Драйверы подключаемого оборудования», XML в UTF-8).
#pragma once

#include <string>
#include <vector>
#include <map>

namespace xmlutil {

// Разбор блока <Parameters><Parameter Name="..." Value="..."/>...</Parameters>.
// Возвращает пары Имя -> Значение. Вложенные/чужие теги игнорируются.
std::map<std::wstring, std::wstring> ParseParameters(const std::wstring& xml);

// Терпеливое извлечение строк для StringOutputOnCustomerDisplay.
// Поддерживает варианты:
//   <Strings><String>текст</String></Strings>
//   <Strings><String Value="текст"/></Strings>
//   <Strings><Line>текст</Line></Strings> / <Строки><Строка>...
//   обычный текст с переносами строк (\n, \r\n)
std::vector<std::wstring> ParseDisplayLines(const std::wstring& data);

// Значение атрибута name у первого тега tag в xml. Пусто, если не найдено.
std::wstring GetTagAttr(const std::wstring& xml, const std::wstring& tag,
                       const std::wstring& name);

// Сборка XML
std::wstring EscapeAttr(const std::wstring& s);   // для значений атрибутов
std::wstring EscapeText(const std::wstring& s);   // для текстовых узлов

// Вспомогательное: значение параметра окружения вида "RRGGBB" -> COLORREF
COLORREF ParseColor(const std::wstring& hex, COLORREF fallback);

} // namespace xmlutil
