# Audyt Bezpieczeństwa i Architektury - BanksConnectApp

Poniżej znajduje się lista potencjalnych błędów, luk bezpieczeństwa oraz problemów architektonicznych zidentyfikowanych w kodzie źródłowym backendu.

## 1. Architektura Sieciowa (HTTP Server)
- **Własna implementacja serwera HTTP** (`app_server.cc:serve`): Zamiast sprawdzonego frameworka (np. drogon, crow, httplib), użyto bezpośrednich wywołań POSIX `socket()`, `accept()`, `read()`, `write()`. Może to łatwo prowadzić do problemów z wyciekami deskryptorów plików (FD leaks), jeśli zapomni się zamknąć połącznie w bloku `catch`.
- **Podatność na Slowloris DDoS**: Synchroniczne zablokowanie wątku podczas czytania nagłówków, bez nałożonych timeoutów na poziomie socketu (`SO_RCVTIMEO`), sprawia, że jeden napastnik wysyłający 1 bajt co 10 sekund zablokuje cały serwer (brak architektury asynchronicznej/epoll).
- **Zarządzanie wątkami**: `std::thread` dla każdego żądania (o ile tak jest to ujęte wewnątrz `serve()`) tworzy ogromny narzut i może wyczerpać pamięć serwera przy ataku typu flood. Należy zastosować pulę wątków (Thread Pool).

## 2. Podatności Bezpieczeństwa (Security)
- **SQL Injection (DIY Query Building)** w `database.cc`: Własna funkcja ucieczki apostrofów `static std::string q(const std::string& v)` i łączenie stringów `exec("SELECT ... WHERE id=" + q(id))` to antywzorzec. Konieczne jest użycie tzw. *Prepared Statements* (bind parameters, `sqlite3_bind_text`), aby zabezpieczyć się przed zaawansowanymi wektorami wstrzyknięć SQL.
- **Wywołanie systemowe do OpenSSL** (`enablebanking_client.cc`): Podpisywanie JWT odbywa się za pomocą tworzenia nowego procesu poprzez `fork()` oraz `execvp("openssl", ...)`. Jest to bardzo nieefektywne (szczególnie pod obciążeniem serwera) i stwarza gigantyczne ryzyko. Powinno się wykorzystać bezpośrednio C++ API biblioteki `libssl` (`EVP_DigestSignInit` itp.) zamiast procesu podrzędnego i potoków.

## 3. Integralność i Obsługa Błędów
- **Wyjątki uciekające z wątków**: Brak globalnego try-catch w funkcji pętli głównej może doprowadzić do nagłego "ubicia" (crashu) całego procesu w C++, gdy jakikolwiek nieobsłużony błąd sieciowy wystąpi na etapie pobierania danych.
- **Brak walidacji danych wejściowych**: Endpointy API wyciągające parametry `jf("name")`, `ji("amount")` ufają, że klient nie przesłał złośliwych danych. Brak walidacji po stronie logiki domenowej np. w `Database::upsertAccount`.

## 4. Baza Danych (SQLite)
- **Zbyt silne blokady dla SQLite**: Co prawda włączono `WAL` (`PRAGMA journal_mode=WAL`), ale brak jest odpowiedniej obsługi kodu błędu `SQLITE_BUSY`. Jeśli jednocześnie aplikacja mobilna będzie wysyłać zapytania POST do kilku edycji, a wątek synchronizacji zacznie przetwarzać tysiące transakcji, to niektóre zapytania zwrócą błąd. Rekomendowane dodanie `sqlite3_busy_timeout()`.

## Rekomendacje:
1. Zamienić manualny socket HTTP na np. bibliotekę `cpp-httplib` (nagłówkowa, mega prosta w dołączeniu).
2. Przepisać zapytania w `database.cc` z łączenia znaków (string concat) na parametryzowane instrukcje `sqlite3_prepare_v2` + `sqlite3_bind_text()`.
3. Zastąpić `fork()` dla openssl wykorzystaniem wbudowanej funkcji kryptograficznej z `openssl/evp.h` lub tokenu generowanego asymetrycznie za pomocą `jwt-cpp`.
