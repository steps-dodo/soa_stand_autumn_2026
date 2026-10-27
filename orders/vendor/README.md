# Сторонние библиотеки

Эти файлы входят в исходники, поэтому сборка не скачивает зависимости с GitHub:

- cpp-httplib **0.18.3**, HTTP-сервер и клиент. Источник: https://github.com/yhirose/cpp-httplib/tree/v0.18.3 . Лицензия MIT в `httplib.LICENSE`.
- nlohmann/json **3.11.3**, JSON. Источник: https://github.com/nlohmann/json/tree/v3.11.3 . Лицензия MIT в `json.LICENSE`.

SHA-256 полученных файлов:

```text
httplib.h a0a0c13dc086663863dbe6730a19716f7d3744904b6205496e8301750814dbd5
json.hpp 9bea4c8066ef4a1c206b2be5a36302f8926f7fdc6087af5d20b417d0cf103ea6
```

Системная зависимость: клиентская библиотека PostgreSQL libpq. В Docker она устанавливается через пакеты Debian; при локальной сборке её путь передаётся CMake.
