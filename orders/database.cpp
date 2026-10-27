// Здесь сосредоточены вызовы PostgreSQL
#include "database.hpp"
#include <memory>
#include <stdexcept>

// Открываем соединение с ограничением ожидания подключения в три секунды
Database::Database(const std::string& url) {
    const char* keys[] = {"dbname", "connect_timeout", nullptr};
    const char* values[] = {url.c_str(), "3", nullptr};
    // Последняя единица разрешает разобрать полный URL подключения, переданный в параметре имени базы
    connection = PQconnectdbParams(keys, values, 1);
    if (connection == nullptr || PQstatus(connection) != CONNECTION_OK) {
        if (connection != nullptr) {
            PQfinish(connection);
        }
        throw std::runtime_error("Cannot connect to PostgreSQL");
    }
}

// Освобождаем подключение при выходе из области видимости объекта
Database::~Database() {
    PQfinish(connection);
}

// Преобразуем параметры для библиотеки, исполняем SQL и копируем результат в строки C++
Rows Database::query(const std::string& sql,
                     const std::vector<std::string>& parameters) {
    std::vector<const char*> values;
    for (const std::string& parameter : parameters) {
        values.push_back(parameter.c_str());
    }

    // Параметры отделены от SQL, поэтому кавычки в данных не превращаются в команды базы
    PGresult* raw = PQexecParams(connection, sql.c_str(),
        static_cast<int>(values.size()), nullptr, values.data(), nullptr, nullptr, 0);
    // Освободит результат через PQclear даже при исключении
    std::unique_ptr<PGresult, decltype(&PQclear)> result(raw, PQclear);
    if (raw == nullptr) {
        throw std::runtime_error("PostgreSQL returned no result");
    }
    // Допускаем успешный запрос со строками и успешную команду без строк. Другие статусы в исключение
    ExecStatusType status = PQresultStatus(raw);
    if (status != PGRES_TUPLES_OK && status != PGRES_COMMAND_OK) {
        throw std::runtime_error(PQresultErrorMessage(raw));
    }

    // Копируем все строки и столбцы, начиная с индекса ноль
    Rows rows;
    for (int row = 0; row < PQntuples(raw); ++row) {
        std::vector<std::string> columns;
        for (int column = 0; column < PQnfields(raw); ++column) {
            columns.push_back(PQgetvalue(raw, row, column));
        }
        rows.push_back(columns);
    }
    return rows;
}

// Задаём начальные состояния и ограничения таблицы
void Database::initialize() {
    // PQexecParams принимает одну SQL-команду за вызов
    query(R"SQL(
        CREATE TABLE IF NOT EXISTS orders (
            id TEXT PRIMARY KEY DEFAULT gen_random_uuid()::text,
            user_id TEXT NOT NULL,
            items JSONB NOT NULL,
            amount_cents BIGINT NOT NULL CHECK (amount_cents > 0),
            status TEXT NOT NULL DEFAULT 'pending'
                CHECK (status IN ('pending', 'paid', 'rejected')),
            stage TEXT NOT NULL DEFAULT 'reserve'
                CHECK (stage IN ('reserve', 'pay', 'release', 'done')),
            attempts INTEGER NOT NULL DEFAULT 0,
            next_attempt_at TIMESTAMPTZ NOT NULL DEFAULT now(),
            created_at TIMESTAMPTZ NOT NULL DEFAULT now()
        )
    )SQL");
    // Индекс включает только незавершённые заказы и ускоряет поиск тех, которым уже пора повторяться
    query("CREATE INDEX IF NOT EXISTS orders_pending "
          "ON orders(next_attempt_at, created_at) WHERE status = 'pending'");

    // Исходящие события переживают перезапуск. id и время создаются один раз, а не при каждой отправке.
    query(R"SQL(
        CREATE TABLE IF NOT EXISTS outbox (
            id TEXT PRIMARY KEY DEFAULT gen_random_uuid()::text,
            order_id TEXT NOT NULL UNIQUE REFERENCES orders(id),
            occurred_at TIMESTAMPTZ NOT NULL DEFAULT now(),
            sent BOOLEAN NOT NULL DEFAULT false,
            attempts INTEGER NOT NULL DEFAULT 0,
            next_attempt_at TIMESTAMPTZ NOT NULL DEFAULT now()
        )
    )SQL");
    query("CREATE INDEX IF NOT EXISTS outbox_pending "
          "ON outbox(next_attempt_at) WHERE sent = false");
    // При обновлении с Task 1 создаём события и для уже оплаченных заказов.
    // Отправленные строки сохраняем, поэтому повторный запуск не создаёт события заново.
    query("INSERT INTO outbox(order_id) SELECT id FROM orders WHERE status = 'paid' "
          "ON CONFLICT(order_id) DO NOTHING");

    // Первичный ключ не даст записать один входящий event_id дважды, даже из разных потоков.
    query(R"SQL(
        CREATE TABLE IF NOT EXISTS inbox (
            event_id TEXT PRIMARY KEY,
            user_id TEXT NOT NULL,
            bonus_points BIGINT NOT NULL CHECK (bonus_points > 0)
        )
    )SQL");
    query(R"SQL(
        CREATE TABLE IF NOT EXISTS bonuses (
            user_id TEXT PRIMARY KEY,
            points BIGINT NOT NULL CHECK (points >= 0)
        )
    )SQL");
}
