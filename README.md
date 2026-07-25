# BanksConnectApp

BanksConnectApp to aplikacja serwerowa w C++ (backend), która służy do integracji z zewnętrznymi interfejsami API banków (korzystając z platformy Enable Banking) oraz do zarządzania domowymi finansami. System autoryzuje użytkownika w wybranych bankach, pobiera w tle informacje o kontach, saldach oraz historii transakcji, a następnie udostępnia ujednolicone dane za pomocą REST API, z którego korzysta aplikacja mobilna (np. napisana w Qt6 / C++). 

Aplikacja wykorzystuje SQLite do lokalnej bazy danych (trwałe przechowywanie transakcji, budżetów, oszczędności itp.).

## Struktura plików i ich role

Aplikacja opiera się na modułowej architekturze, gdzie każda klasa i plik pełni określoną rolę:

### Punkt wejścia
* **`main.cpp`** - Punkt wejścia aplikacji. Wczytuje i parsuje zmienne środowiskowe, ładuje konfigurację i uruchamia główny serwer HTTP (`AppServer`). Obsługuje również proste wywołania z CLI (np. flaga `--auth-url`).

### Logika serwera i API
* **`app_server.h` / `app_server.cc`** - Główny silnik aplikacji. Klasa `AppServer` implementuje:
  * Prosty serwer HTTP (routing żądań i autoryzacja).
  * Webhooki i callbacki OAuth2.
  * Pętlę działającą w tle (`syncThread_`), która systematycznie synchronizuje dane z serwerami banków.
  * Wystawianie **REST API** (`/api/db/*`) dla aplikacji mobilnej z zabezpieczeniem Bearer Token.
* **`money.h` / `money.cc`** - Parsowanie i formatowanie kwot w jednostkach podrzędnych (grosze) na liczbach całkowitych, bez pośredniego `double`.

### Komunikacja zewnętrzna (Enable Banking)
* **`enablebanking_client.h` / `enablebanking_client.cc`** - Klient do połączeń z API dostawcy Enable Banking. Odpowiada za:
  * Budowanie żądań, szyfrowanie, generowanie i podpisywanie tokenów JWT (przy użyciu OpenSSL).
  * Rozpoczęcie procesu autoryzacji z bankiem (generowanie linku logowania).
  * Odbiór sesji, pobieranie sald (`/balances`), szczegółów kont (`/details`) i historii transakcji (`/transactions`).

### Model Danych i Baza SQLite
* **`database.h` / `database.cc`** - Warstwa dostępu do bazy danych (SQLite wrapper). Zawiera zapytania SQL, które pozwalają na dodawanie, modyfikację i usuwanie (CRUD) takich bytów jak konta, transakcje (wraz ze statusem edycji, czy ich rozbijaniem tzw. splitem), cele oszczędnościowe, budżety oraz zadania (todos). W bazie przetrzymywana jest też historia synchronizacji.
* **`acc.h` / `acc.cc`** - Definicja i struktura zwykłego konta bankowego.
* **`acc_invest.h` / `acc_invest.cc`** - Rozszerzona definicja dla kont inwestycyjnych (przechowuje informacje o inwestycjach i okresach inwestowania).
* **`trans.h`** - Definicja struktury transakcji bankowej, wraz z potężnymi enumami kategoryzującymi (np. tagi typu `must`, `opt`, `waste`, typy, czy kody walut).

### Transformacja Danych
* **`json_mapper.h` / `json_mapper.cc`** - Parsowanie odpowiedzi JSON ze standardu dostarczanego przez Enable Banking i tłumaczenie ich na natywne, wbudowane obiekty i struktury danych napisane w C++ (jak `trans`, `acc`, salda).

---

## Data Flow (Przepływ danych)

Jak dokładnie wyglądają przepływy danych w aplikacji i integracja banków?

1. **Inicjacja i Autoryzacja:**
   - Użytkownik wchodzi na stronę główną (wystawianą przez `AppServer` pod ścieżką `/`) i wybiera swój bank (np. mBank, Santander, itp.).
   - Żądanie ląduje na `/start-auth`, gdzie `EnableBankingClient` na podstawie konfiguracji generuje bezpieczny URL przekierowujący użytkownika na portal logowania danego banku.
2. **Logowanie i Callback:**
   - Po pomyślnym zalogowaniu się użytkownika na stronie banku, bank wraca z powrotem na wskazany w konfiguracji Endpoint (`/oauth/callback`) z kodem autoryzacyjnym (`code`).
   - Serwer wymienia go z platformą Enable Banking na aktywną **sesję** (zawierającą m.in. identyfikatory kont użytkownika).
3. **Synchronizacja w tle (Sync):**
   - Wbudowany w `AppServer` pętlowy wątek działa w tle. Regularnie przechodzi po wszystkich zapisanych sesjach i poprzez `EnableBankingClient` odpytuje bank o listę rachunków, salda i nowe transakcje.
   - Odpowiedź banku trafia w formie czystego JSON-a do modułu `json_mapper`.
   - `json_mapper` parsuje to na obiekty `acc` i `trans`.
4. **Zapis do bazy danych:**
   - Sparsowane obiekty trafiają do logiki `Database`, która zarządza zapisem do pliku lokalnego SQLite (`upsertAccount`, `insertTx` itp.). Baza zaciąga zmiany i nie dubluje transakcji dzięki przechowywaniu identyfikatorów bezpośrednio z banku (`bankTxId`).

---

## Co jest wystawiane dla aplikacji Mobile i jak aplikacja Mobile tego używa?

Backend w pełni wystawia ujednolicony interfejs API (REST) komunikujący się przy użyciu formatu JSON. Aplikacja **Mobile** (użytkownika) wykorzystuje go do pełnego wglądu i zarządzania swoimi zasobami.

### Autoryzacja dla Mobile:
Aby aplikacja mobilna mogła się połączyć, musi podać nagłówek autoryzacyjny:
`Authorization: Bearer <TWÓJ_TOKEN_Z_ENV>` (token ustawiony w zmiennych środowiskowych serwera).

### Kluczowe wystawiane endpointy wykorzystywane przez Mobile:
* **`GET /api/db/accounts`** - Pobranie listy kont bankowych oraz ich aktualnych sald do wyrysowania widoku.
* **`GET /api/db/transactions`** - Pobieranie zapisanej i zagregowanej historii transakcji, która zawiera również informacje o ew. rozbiciach (split) lub tagach. Posiada możliwość filtrowania.
* **`PUT / DELETE /api/db/transactions/{id}`** - Służy do modyfikacji danej transakcji w UI (np. użytkownik mobilny zmienia tag z `must` na `waste`, co zapisywane jest w bazie) bądź jej usunięcia.
* **`POST /api/db/transactions/{id}/split`** - Aplikacja mobilna może podzielić dużą transakcję z banku (np. paragony za zakupy i chemię) na kilka pod-transakcji dla lepszego zarządzania budżetem.
* **`GET / POST /api/db/categories`** - Zarządzanie własnymi kategoriami.
* **`GET / POST /api/db/budgets`** - Podsumowania i ustawienia limitów dla danych miesięcy (np. limit na jedzenie). Endpoint `/api/db/budgets/summary` zwraca informacje o stopniu realizacji (zaplanowane wydatki kontra rzeczywiste).
* **`GET / POST /api/db/savings` oraz `/api/db/savings/{id}/chart`** - Zwraca mobilce dane na temat celów oszczędnościowych i progresu miesiąc do miesiąca (rysowanie wykresów).
* **`GET /api/db/todos`** - Zarządzanie cyklicznymi rachunkami do zapłacenia / rzeczami do zrobienia w finansach.
* **`GET /api/db/stats`** - Wyciąganie statystyk ułożonych po kategoriach na potrzebę wyświetlania grafów w telefonie.
* **`POST /api/db/sync/now`** - Ręczne (na żądanie) wyzwolenie pełnej synchronizacji z serwerami banku za pomocą "Pull-to-refresh" lub kliknięcia przycisku w Mobile.

Cały system jest skonstruowany tak, aby zminimalizować bezpośrednie połączenia klienta mobilnego do banku (Mobile gada tylko z C++ Backendem), podczas gdy serwer przejmuje ciężar autoryzacji, transformacji typów i regularnego fetchowania danych.
