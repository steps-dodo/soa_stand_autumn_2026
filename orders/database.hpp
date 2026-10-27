// Небольшая оболочка над libpq: соединение, выполнение SQL и создание таблицы заказов.
#pragma once
#include <libpq-fe.h>
#include <string>
#include <vector>

// Результат SELECT: список строк; каждая строка — список значений столбцов.
using Rows = std::vector<std::vector<std::string>>;

// Каждый объект владеет одним подключением и закрывает его при уничтожении.
class Database {
public:
    explicit Database(const std::string& url);
    ~Database();
    // Копирование запрещено: два владельца одного указателя могли бы закрыть подключение дважды.
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    // Выполняем одну SQL-команду. Значения параметров передаются отдельно от текста запроса.
    Rows query(const std::string& sql,
               const std::vector<std::string>& parameters = {});
    // Создаём таблицу и индекс при первом запуске; уже существующие данные сохраняются.
    void initialize();

private:
    PGconn* connection = nullptr;
};
