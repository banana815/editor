#include "zotero.h"
#include "cJSON.h"
#include <sqlite3.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include <wchar.h>
#endif

static FILE *open_read(const char *path) {
#ifdef _WIN32
    wchar_t wide[2048];
    assert(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, 2048));
    return _wfopen(wide, L"rb");
#else
    return fopen(path, "rb");
#endif
}

static void remove_path(const char *path) {
#ifdef _WIN32
    wchar_t wide[2048];
    assert(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, 2048));
    _wremove(wide);
#else
    remove(path);
#endif
}

static void execute(sqlite3 *db, const char *sql) {
    char *error = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &error);
    if (rc != SQLITE_OK) fprintf(stderr, "Fixture SQL: %s\n", error);
    sqlite3_free(error);
    assert(rc == SQLITE_OK);
}

static sqlite3 *create_db(const char *path) {
    sqlite3 *db = NULL;
    remove_path(path);
    assert(sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL) == SQLITE_OK);
    return db;
}

static const char *common_schema =
    "CREATE TABLE itemTypes(itemTypeID INTEGER PRIMARY KEY,typeName TEXT);"
    "INSERT INTO itemTypes VALUES(1,'book'),(2,'attachment'),(3,'note'),(4,'annotation');"
    "CREATE TABLE fields(fieldID INTEGER PRIMARY KEY,fieldName TEXT);"
    "INSERT INTO fields VALUES(1,'title'),(2,'date');"
    "CREATE TABLE itemData(itemID INTEGER,fieldID INTEGER,valueID INTEGER);"
    "CREATE TABLE itemDataValues(valueID INTEGER PRIMARY KEY,value TEXT);"
    "CREATE TABLE itemCreators(itemID INTEGER,creatorID INTEGER,creatorTypeID INTEGER,orderIndex INTEGER);"
    "CREATE TABLE collectionItems(collectionID INTEGER,itemID INTEGER);";

static void modern_fixture(sqlite3 *db) {
    execute(db, common_schema);
    /* Columns and relationships follow Zotero's modern userdata.sql, with
     * system itemTypes/fields above. Group ID differs from local library ID. */
    execute(db,
        "CREATE TABLE libraries(libraryID INTEGER PRIMARY KEY,type TEXT);"
        "INSERT INTO libraries VALUES(1,'user'),(9,'group'),(12,'feed');"
        "CREATE TABLE groups(groupID INTEGER PRIMARY KEY,libraryID INTEGER UNIQUE);"
        "INSERT INTO groups VALUES(112233,9);"
        "CREATE TABLE items(itemID INTEGER PRIMARY KEY,itemTypeID INTEGER,key TEXT,libraryID INTEGER);"
        "INSERT INTO items VALUES(1,1,'ABCDEFGH',1),(2,1,'ABCDEFGH',9),(3,1,'DELETED1',1),"
        "(4,3,'NOTE0001',1),(5,4,'ANNOT001',1),(6,1,'FEED0001',12),"
        "(10,2,'PDF00001',1),(11,2,'TRASH001',1),(12,2,'HTML0001',1),"
        "(13,2,'PDF00002',9),(14,2,'URL00001',9);"
        "INSERT INTO itemDataValues VALUES(1,'中文书名'),(2,'2024-03-14'),(3,'Group book');"
        "INSERT INTO itemData VALUES(1,1,1),(1,2,2),(2,1,3);"
        "CREATE TABLE creators(creatorID INTEGER PRIMARY KEY,firstName TEXT,lastName TEXT,fieldMode INTEGER);"
        "INSERT INTO creators VALUES(1,'三','张',0),(2,'ignored','研究机构',1);"
        "INSERT INTO itemCreators VALUES(1,2,1,1),(1,1,1,0);"
        "CREATE TABLE deletedItems(itemID INTEGER PRIMARY KEY);"
        "INSERT INTO deletedItems VALUES(3),(11);"
        "CREATE TABLE itemAttachments(itemID INTEGER PRIMARY KEY,parentItemID INTEGER,contentType TEXT,linkMode INTEGER);"
        "INSERT INTO itemAttachments VALUES(10,1,'application/pdf',0),(11,1,'application/pdf',0),"
        "(12,1,'text/html',1),(13,2,'application/pdf',2),(14,2,'application/pdf',3);"
        "CREATE TABLE collections(collectionID INTEGER PRIMARY KEY,collectionName TEXT,parentCollectionID INTEGER,key TEXT,libraryID INTEGER);"
        "INSERT INTO collections VALUES(1,'root',NULL,'COL00001',1),(2,'child',1,'COL00002',1),"
        "(3,'cycle A',4,'COL00003',1),(4,'cycle B',3,'COL00004',1),"
        "(5,'orphan',999,'COL00005',1),(6,'group',NULL,'COL00006',9),"
        "(7,'wrong library parent',6,'COL00007',1),(8,'self',8,'COL00008',1),"
        "(50,'trashed',NULL,'COL00050',1);"
        "CREATE TABLE deletedCollections(collectionID INTEGER PRIMARY KEY);"
        "INSERT INTO deletedCollections VALUES(50);"
        "INSERT INTO collectionItems VALUES(1,1),(1,1),(2,1),(2,3),(2,4),(2,10),"
        "(6,2),(1,2),(50,1);");
}

static void legacy_fixture(sqlite3 *db) {
    execute(db, common_schema);
    execute(db,
        "CREATE TABLE items(itemID INTEGER PRIMARY KEY,itemTypeID INTEGER,key TEXT);"
        "INSERT INTO items VALUES(1,1,'LEGACY01');"
        "INSERT INTO itemDataValues VALUES(1,'Old book'),(2,'Spring 1999');"
        "INSERT INTO itemData VALUES(1,1,1),(1,2,2);"
        "CREATE TABLE creators(creatorID INTEGER PRIMARY KEY,creatorDataID INTEGER);"
        "CREATE TABLE creatorData(creatorDataID INTEGER PRIMARY KEY,firstName TEXT,lastName TEXT,fieldMode INTEGER);"
        "INSERT INTO creators VALUES(1,100),(2,101);"
        "INSERT INTO creatorData VALUES(100,'Ada','Lovelace',0),(101,'','Legacy Institute',1);"
        "INSERT INTO itemCreators VALUES(1,1,1,0),(1,2,1,1);"
        "CREATE TABLE collections(collectionID INTEGER PRIMARY KEY,collectionName TEXT,parentCollectionID INTEGER);"
        "INSERT INTO collections VALUES(1,'legacy root',NULL);"
        "INSERT INTO collectionItems VALUES(1,1);");
}

static unsigned char *snapshot(const char *path, size_t *size) {
    FILE *file = open_read(path);
    assert(file);
    assert(!fseek(file, 0, SEEK_END));
    long length = ftell(file);
    assert(length >= 0);
    rewind(file);
    unsigned char *data = malloc((size_t)length + 1);
    assert(data);
    assert(fread(data, 1, (size_t)length, file) == (size_t)length);
    assert(!fclose(file));
    *size = (size_t)length;
    return data;
}

static void test_sqlite(const char *path) {
    CeZoteroLibrary lib = {0};
    char error[256];
    sqlite3 *db = create_db(path);
    modern_fixture(db);
    assert(sqlite3_close(db) == SQLITE_OK);
    size_t before_size, after_size;
    unsigned char *before = snapshot(path, &before_size);
    assert(ce_zotero_load_sqlite(path, &lib, error, sizeof error));
    assert(!*error && lib.item_count == 3 && lib.collection_count == 8);
    assert(lib.items[0].database_id == 1 && lib.items[1].database_id == 2);
    assert(!strcmp(lib.items[0].title, "中文书名"));
    assert(!strcmp(lib.items[0].author, "张 三; 研究机构"));
    assert(!strcmp(lib.items[0].year, "2024"));
    assert(!strcmp(lib.items[0].item_uri, "zotero://select/library/items/ABCDEFGH"));
    assert(!strcmp(lib.items[1].item_uri, "zotero://select/groups/112233/items/ABCDEFGH"));
    assert(!strcmp(lib.items[0].pdf_uri, "zotero://open-pdf/library/items/PDF00001"));
    assert(!strcmp(lib.items[1].pdf_uri, "zotero://open-pdf/groups/112233/items/PDF00002"));
    assert(!*lib.items[2].item_uri && !*lib.items[2].pdf_uri); /* Feed is not user library. */
    assert(lib.repaired_parents == 4);
    size_t *indices = NULL, count = 0;
    assert(ce_zotero_collection_items(&lib, 0, true, &indices, &count, error, sizeof error));
    assert(count == 1 && indices[0] == 0); /* Duplicates/deleted/attachment/cross-library excluded. */
    free(indices);
    assert(ce_zotero_collection_items(&lib, 5, false, &indices, &count, error, sizeof error));
    assert(count == 1 && indices[0] == 1);
    free(indices);
    assert(!ce_zotero_collection_items(&lib, 999, true, &indices, &count, error, sizeof error));
    assert(!indices && !count);
    unsigned char *after = snapshot(path, &after_size);
    assert(before_size == after_size && !memcmp(before, after, before_size));
    free(before); free(after);

    /* Failure after items have been read must preserve the previous library. */
    CeZoteroItem *original = lib.items;
    assert(sqlite3_open(path, &db) == SQLITE_OK);
    execute(db, "UPDATE creators SET lastName=CAST(x'ff' AS TEXT) WHERE creatorID=1");
    assert(!ce_zotero_load_sqlite(path, &lib, error, sizeof error));
    assert(lib.items == original && lib.item_count == 3 && strstr(error, "UTF-8"));
    /* The failed read transaction must be closed: a writer can proceed. */
    execute(db, "BEGIN EXCLUSIVE; UPDATE creators SET lastName='张' WHERE creatorID=1; COMMIT;");
    execute(db, "UPDATE itemDataValues SET value='bad' || char(0) || 'value' WHERE valueID=1");
    assert(!ce_zotero_load_sqlite(path, &lib, error, sizeof error));
    assert(lib.items == original && strstr(error, "NUL"));
    execute(db, "DROP TABLE itemCreators");
    assert(!ce_zotero_load_sqlite(path, &lib, error, sizeof error));
    assert(lib.items == original && strstr(error, "itemCreators"));
    assert(sqlite3_close(db) == SQLITE_OK);
    ce_zotero_library_free(&lib);
    assert(!lib.items && !lib.item_count && !lib.collections);

    db = create_db(path);
    legacy_fixture(db);
    assert(sqlite3_close(db) == SQLITE_OK);
    assert(ce_zotero_load_sqlite(path, &lib, error, sizeof error));
    assert(lib.item_count == 1 && !strcmp(lib.items[0].title, "Old book"));
    assert(!strcmp(lib.items[0].author, "Lovelace Ada; Legacy Institute"));
    assert(!strcmp(lib.items[0].year, "1999"));
    assert(!*lib.items[0].pdf_uri);
    ce_zotero_library_free(&lib);
    remove_path(path);
    assert(!ce_zotero_load_sqlite(path, &lib, error, sizeof error));
    FILE *missing = open_read(path);
    assert(!missing); /* READONLY must not create a missing database. */
}

static void test_unicode_path(const char *path) {
    sqlite3 *db = create_db(path);
    legacy_fixture(db);
    execute(db, "UPDATE creatorData SET firstName=NULL,lastName=NULL;"
                "UPDATE itemDataValues SET value=NULL WHERE valueID=2;");
    assert(sqlite3_close(db) == SQLITE_OK);
    size_t before_size, after_size;
    unsigned char *before = snapshot(path, &before_size);
    CeZoteroLibrary lib = {0};
    char error[128];
    assert(ce_zotero_load_sqlite(path, &lib, error, sizeof error));
    assert(lib.item_count == 1);
    assert(!strcmp(lib.items[0].author, "未知作者") && !*lib.items[0].year);
    ce_zotero_library_free(&lib);
    unsigned char *after = snapshot(path, &after_size);
    assert(before_size == after_size && !memcmp(before, after, before_size));
    free(before); free(after);
    remove_path(path);
}

static void test_csl(void) {
    CeZoteroLibrary lib = {0};
    char error[256];
    const char *json = "[{\"id\":\"Smith2024\",\"title\":\" 中文标题 \","
        "\"author\":[{\"family\":\"张\",\"given\":\"三\"},{\"literal\":\"研究院\"}],"
        "\"issued\":{\"date-parts\":[[2024,3,14]]},\"citationKey\":\"Smith2024\","
        "\"DOI\":\"10.example/test\",\"publisher\":\"出版社\"},"
        "{\"id\":\"http://zotero.org/groups/112233/items/ABCDEFGH\",\"title\":\"Group book\","
        "\"pdfUri\":\"zotero://open-pdf/groups/112233/items/PDF00002\"},"
        "{\"id\":12345678},{\"id\":\"ABCDEFGH\"}]";
    assert(ce_zotero_import_csl(json, &lib, error, sizeof error));
    assert(lib.item_count == 4 && !*error);
    assert(!strcmp(lib.items[0].title, "中文标题"));
    assert(!strcmp(lib.items[0].author, "张 三; 研究院"));
    assert(!strcmp(lib.items[0].year, "2024"));
    assert(!*lib.items[0].item_key && !*lib.items[0].item_uri && !*lib.items[0].pdf_uri);
    assert(!strcmp(lib.items[1].item_uri, "zotero://select/groups/112233/items/ABCDEFGH"));
    assert(!strcmp(lib.items[1].pdf_uri, "zotero://open-pdf/groups/112233/items/PDF00002"));
    assert(!strcmp(lib.items[2].id, "12345678") && !*lib.items[2].item_uri);
    assert(!strcmp(lib.items[3].item_key, "ABCDEFGH") && !*lib.items[3].pdf_uri);
    cJSON *preserved = cJSON_Parse(lib.items[0].csl_json);
    assert(preserved && !strcmp(cJSON_GetObjectItemCaseSensitive(preserved, "DOI")->valuestring, "10.example/test"));
    cJSON_Delete(preserved);
    CeZoteroItem *original = lib.items;
    const char *invalid[] = {"{}", "[null]", "[{\"author\":5}]", "[{\"title\":\"a\\u0000b\"}]",
                            "[{\"title\":\"\xff\"}]", "[{\"issued\":{\"date-parts\":4}}]", "[] trailing"};
    for (size_t i = 0; i < sizeof invalid / sizeof *invalid; i++) {
        assert(!ce_zotero_import_csl(invalid[i], &lib, error, sizeof error));
        assert(lib.items == original && lib.item_count == 4 && *error);
    }
    char *citation = ce_zotero_format_approximate(&lib.items[0], 1, CE_ZOTERO_APPROXIMATE_APA);
    assert(citation && strstr(citation, "2024") && strstr(citation, "中文标题"));
    free(citation);
    assert(!ce_zotero_format_approximate(&lib.items[0], 1, (CeZoteroApproximateStyle)99));
    assert(ce_zotero_import_csl("[{\"id\":\"\",\"URL\":\"\"},"
        "{\"id\":\"https://zotero.org/groups/x/items/ABCDEFGH\"}]", &lib, error, sizeof error));
    assert(lib.item_count == 2 && !*lib.items[0].item_uri && !*lib.items[1].item_uri);
    assert(ce_zotero_import_csl("[]", &lib, error, sizeof error));
    assert(!lib.item_count && !lib.items);
    ce_zotero_library_free(&lib);
}

static int run_tests(const char *directory) {
    char path[2048], unicode_path[2048];
    assert(snprintf(path, sizeof path, "%s/test-zotero.sqlite", directory) < (int)sizeof path);
    assert(snprintf(unicode_path, sizeof unicode_path, "%s/test-zotero-中文.sqlite", directory) < (int)sizeof unicode_path);
    test_csl();
    test_sqlite(path);
    test_unicode_path(unicode_path);
    puts("Zotero CSL, modern/legacy SQLite, read-only, atomic replacement and collections passed");
    return 0;
}

#ifdef _WIN32
int wmain(int argc, wchar_t **argv) {
    assert(argc == 2); /* CTest supplies a disposable build directory. */
    /* /utf-8 governs literals, not the CRT's narrow argv. Keep arguments wide
     * until this explicit conversion so non-ACP directory names survive. */
    char directory[8192];
    assert(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[1], -1,
                              directory, sizeof directory, NULL, NULL));
    return run_tests(directory);
}
#else
int main(int argc, char **argv) {
    assert(argc == 2);
    return run_tests(argv[1]);
}
#endif
