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

static void test_cache_roundtrip(const CeZoteroLibrary *expected) {
    CeZoteroLibrary restored={0}; char error[256];
    char *cache=ce_zotero_cache_export(expected,error,sizeof error);
    assert(cache && !*error && ce_zotero_cache_import(cache,&restored,error,sizeof error));
    assert(restored.item_count==expected->item_count && restored.collection_count==expected->collection_count);
    assert(restored.repaired_parents==expected->repaired_parents && restored.root_count==expected->root_count);
    char *again=ce_zotero_cache_export(&restored,error,sizeof error);
    assert(again && !strcmp(cache,again)); free(again);
    CeZoteroItem *retained=restored.items;
    cJSON *broken=cJSON_Parse(cache); assert(broken);
    cJSON_ReplaceItemInObjectCaseSensitive(broken,"version",cJSON_CreateNumber(2));
    char *invalid=cJSON_PrintUnformatted(broken); assert(invalid);
    assert(!ce_zotero_cache_import(invalid,&restored,error,sizeof error));
    assert(restored.items==retained && restored.item_count==expected->item_count);
    free(invalid); cJSON_Delete(broken);
    if (restored.item_count) {
        broken=cJSON_Parse(cache);
        cJSON *row=cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(broken,"items"),0);
        cJSON_ReplaceItemInObjectCaseSensitive(row,"csl_json",cJSON_CreateString("{broken"));
        invalid=cJSON_PrintUnformatted(broken);
        assert(invalid && !ce_zotero_cache_import(invalid,&restored,error,sizeof error));
        assert(restored.items==retained); free(invalid); cJSON_Delete(broken);
        static const char *bad_ids[]={"9223372036854775808","-9223372036854775809",
            "1e3","0x10","-","+1"," 1","1 ","1.0",""};
        for (size_t i=0;i<sizeof bad_ids/sizeof *bad_ids;++i) {
            broken=cJSON_Parse(cache); assert(broken);
            row=cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(broken,"items"),0);
            assert(cJSON_ReplaceItemInObjectCaseSensitive(row,"database_id",cJSON_CreateString(bad_ids[i])));
            invalid=cJSON_PrintUnformatted(broken); assert(invalid);
            assert(!ce_zotero_cache_import(invalid,&restored,error,sizeof error));
            assert(*error && restored.items==retained); free(invalid); cJSON_Delete(broken);
        }
    }
    if (restored.collection_count) {
        broken=cJSON_Parse(cache);
        cJSON *row=cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(broken,"collections"),0);
        cJSON *members=cJSON_CreateArray(); assert(members);
        assert(cJSON_AddItemToArray(members,cJSON_CreateNumber(999999)));
        cJSON_ReplaceItemInObjectCaseSensitive(row,"items",members);
        invalid=cJSON_PrintUnformatted(broken);
        assert(invalid && !ce_zotero_cache_import(invalid,&restored,error,sizeof error));
        assert(restored.items==retained); free(invalid); cJSON_Delete(broken);
        static const char *bad_members[]={"[0,0]","[-1]","[0.5]","[\"0\"]","[null]"};
        for (size_t i=0;i<sizeof bad_members/sizeof *bad_members;++i) {
            broken=cJSON_Parse(cache); assert(broken);
            row=cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(broken,"collections"),0);
            assert(cJSON_ReplaceItemInObjectCaseSensitive(row,"items",cJSON_Parse(bad_members[i])));
            invalid=cJSON_PrintUnformatted(broken); assert(invalid);
            assert(!ce_zotero_cache_import(invalid,&restored,error,sizeof error));
            assert(*error && restored.items==retained); free(invalid); cJSON_Delete(broken);
        }
    }
    assert(!ce_zotero_cache_import("{\"version\":1,\"items\":[{\"id\":\"a\\u0000b\"}],\"collections\":[]}",&restored,error,sizeof error));
    assert(restored.items==retained);
    free(cache); ce_zotero_library_free(&restored);
}

static void test_snapshot_import(const unsigned char *data, size_t size,
                                  const CeZoteroLibrary *expected) {
    CeZoteroLibrary imported = {0};
    char error[256];
    unsigned char *original = malloc(size);
    assert(original); memcpy(original, data, size);
    assert(ce_zotero_load_sqlite_bytes(data, size, &imported, error, sizeof error));
    assert(!*error && imported.item_count == expected->item_count);
    assert(imported.collection_count == expected->collection_count);
    assert(imported.repaired_parents == expected->repaired_parents);
    for (size_t i = 0; i < imported.item_count; ++i) {
        assert(!strcmp(imported.items[i].title, expected->items[i].title));
        assert(!strcmp(imported.items[i].author, expected->items[i].author));
        assert(!strcmp(imported.items[i].item_uri, expected->items[i].item_uri));
        assert(!strcmp(imported.items[i].pdf_uri, expected->items[i].pdf_uri));
        assert(imported.items[i].csl_json && expected->items[i].csl_json);
        assert(!strcmp(imported.items[i].csl_json, expected->items[i].csl_json));
    }
    assert(!memcmp(original, data, size));
    CeZoteroItem *retained = imported.items;
    assert(!ce_zotero_load_sqlite_bytes(NULL, size, &imported, error, sizeof error));
    assert(!ce_zotero_load_sqlite_bytes(data, 99, &imported, error, sizeof error));
    assert(!ce_zotero_load_sqlite_bytes(data, 100, &imported, error, sizeof error));
    assert(!ce_zotero_load_sqlite_bytes(data, (size_t)128*1024*1024 + 1,
                                        &imported, error, sizeof error));
    assert(imported.items == retained && imported.item_count == expected->item_count);
    /* A selected WAL-mode main file can omit committed journal data. Reject it
     * instead of silently changing its header to make deserialize accept it. */
    original[18] = original[19] = 2;
    assert(!ce_zotero_load_sqlite_bytes(original, size, &imported, error, sizeof error));
    assert(strstr(error, "WAL") && imported.items == retained);
    free(original);
    test_cache_roundtrip(&imported);
    ce_zotero_library_free(&imported);
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
    test_snapshot_import(before, before_size, &lib);
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
    before = snapshot(path, &before_size);
    test_snapshot_import(before, before_size, &lib);
    free(before);
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

static cJSON *metadata_for(const CeZoteroLibrary *library, int64_t id) {
    for (size_t i = 0; i < library->item_count; ++i)
        if (library->items[i].database_id == id) {
            cJSON *data = cJSON_Parse(library->items[i].csl_json);
            assert(data); return data;
        }
    assert(!"Missing fixture item"); return NULL;
}
static const char *json_text(cJSON *object, const char *key) {
    const char *value = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object,key));
    assert(value); return value;
}
static void test_metadata(const char *path) {
    sqlite3 *db = create_db(path);
    modern_fixture(db);
    execute(db,
        "CREATE TABLE creatorTypes(creatorTypeID INTEGER PRIMARY KEY,creatorType TEXT);"
        "INSERT INTO creatorTypes VALUES(1,'author'),(2,'editor'),(3,'translator'),(4,'inventor'),(5,'unmappedRole');"
        "INSERT INTO creators VALUES(3,'编','李',0),(4,'译','王',0),(5,'明','赵',0),(6,'其','钱',0);"
        "INSERT INTO itemCreators VALUES(1,3,2,2),(1,4,3,3),(1,6,5,4);"
        "INSERT INTO itemTypes VALUES(5,'journalArticle'),(6,'patent'),(7,'case'),(8,'bookSection'),(9,'thesis');"
        "INSERT INTO items VALUES(20,5,'JOURNAL1',1),(21,7,'CASE0001',1),(22,6,'PATENT01',1),"
        "(23,9,'THESIS01',1),(24,8,'CHAPTER1',1);"
        "INSERT INTO itemCreators VALUES(22,5,4,0);"
        "INSERT INTO fields VALUES(3,'DOI'),(4,'publicationTitle'),(5,'volume'),(6,'issue'),(7,'pages'),"
        "(8,'publisher'),(9,'place'),(10,'bookTitle'),(11,'caseName'),(12,'dateDecided'),(13,'issueDate'),"
        "(14,'university'),(15,'citationKey'),(16,'extra'),(17,'customField');"
        "INSERT INTO itemDataValues VALUES(100,'期刊文章'),(101,'10.1234/example'),(102,'学术期刊'),"
        "(103,'12'),(104,'3'),(105,'10–20'),(106,'案例名称'),(107,'2023-02-29'),"
        "(108,'专利名称'),(109,'2024-02-29'),(110,'某大学'),(111,'论文'),(112,'章节'),"
        "(113,'图书全名'),(114,'Key2024'),(115,'原始附加信息'),(116,'保留未知字段');"
        "INSERT INTO itemData VALUES(20,1,100),(20,3,101),(20,4,102),(20,5,103),(20,6,104),(20,7,105),"
        "(20,15,114),(20,16,115),(20,17,116),(21,11,106),(21,12,107),"
        "(22,1,108),(22,13,109),(23,1,111),(23,14,110),(24,1,112),(24,10,113);");
    assert(sqlite3_close(db) == SQLITE_OK);
    CeZoteroLibrary library = {0}; char error[256];
    assert(ce_zotero_load_sqlite(path,&library,error,sizeof error));
    cJSON *item = metadata_for(&library,1);
    assert(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(item,"author")) == 2);
    assert(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(item,"editor")) == 1);
    assert(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(item,"translator")) == 1);
    assert(!strcmp(library.items[0].author,"张 三; 研究机构"));
    cJSON *raw = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(item,"custom"),"zotero");
    assert(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(raw,"creators")) == 5);
    cJSON_Delete(item);
    item = metadata_for(&library,20);
    assert(!strcmp(json_text(item,"type"),"article-journal"));
    assert(!strcmp(json_text(item,"DOI"),"10.1234/example"));
    assert(!strcmp(json_text(item,"container-title"),"学术期刊"));
    assert(!strcmp(json_text(item,"volume"),"12") && !strcmp(json_text(item,"issue"),"3"));
    assert(!strcmp(json_text(item,"page"),"10–20") && !strcmp(json_text(item,"citation-key"),"Key2024"));
    raw = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(item,"custom"),"zotero");
    assert(!strcmp(json_text(cJSON_GetObjectItemCaseSensitive(raw,"fields"),"customField"),"保留未知字段"));
    cJSON_Delete(item);
    item = metadata_for(&library,21);
    assert(!strcmp(json_text(item,"type"),"legal_case") && !strcmp(json_text(item,"title"),"案例名称"));
    assert(!strcmp(json_text(cJSON_GetObjectItemCaseSensitive(item,"issued"),"raw"),"2023-02-29"));
    cJSON_Delete(item);
    item = metadata_for(&library,22);
    assert(!strcmp(json_text(item,"type"),"patent"));
    cJSON *parts = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(item,"issued"),"date-parts"),0);
    assert(cJSON_GetArraySize(parts)==3 && cJSON_GetArrayItem(parts,0)->valueint==2024 && cJSON_GetArrayItem(parts,2)->valueint==29);
    assert(!strcmp(json_text(cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(item,"author"),0),"family"),"赵"));
    cJSON_Delete(item);
    item = metadata_for(&library,23);
    assert(!strcmp(json_text(item,"publisher"),"某大学")); cJSON_Delete(item);
    item = metadata_for(&library,24);
    assert(!strcmp(json_text(item,"type"),"chapter") && !strcmp(json_text(item,"container-title"),"图书全名"));
    cJSON_Delete(item);
    size_t length; unsigned char *bytes = snapshot(path,&length);
    test_snapshot_import(bytes,length,&library); free(bytes);
    ce_zotero_library_free(&library); remove_path(path);
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
    test_cache_roundtrip(&lib);
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
    test_cache_roundtrip(&lib);
    ce_zotero_library_free(&lib);
}

static void test_cache_integer_precision(void) {
    CeZoteroLibrary library={0}, restored={0}; char error[256];
    assert(ce_zotero_import_csl("[{\"id\":\"same\",\"title\":\"Exact IDs\"}]",&library,error,sizeof error));
    library.items[0].database_id=INT64_MIN; library.items[0].library_id=INT64_MAX;
    library.collections=calloc(2,sizeof *library.collections); assert(library.collections);
    library.collection_count=2;
    library.collections[0].id=INT64_MIN; library.collections[1].id=INT64_MAX;
    library.collections[0].library_id=library.collections[1].library_id=INT64_MAX;
    library.collections[0].has_parent=true; library.collections[0].parent_id=INT64_MAX;
    library.collections[0].item_indices=calloc(2,sizeof(size_t)); assert(library.collections[0].item_indices);
    library.collections[0].item_count=1;
    char *cache=ce_zotero_cache_export(&library,error,sizeof error);
    assert(cache && strstr(cache,"9223372036854775807") && strstr(cache,"-9223372036854775808"));
    assert(ce_zotero_cache_import(cache,&restored,error,sizeof error));
    assert(restored.items[0].database_id==INT64_MIN && restored.items[0].library_id==INT64_MAX);
    assert(restored.collections[0].id==INT64_MIN && restored.collections[1].id==INT64_MAX);
    assert(restored.collections[0].parent_id==INT64_MAX && restored.collections[0].parent_index==1);
    assert(restored.root_count==1 && restored.roots[0]==1 && restored.collections[1].child_count==1);
    assert(restored.collections[1].children[0]==0 && restored.collections[0].item_indices[0]==0);
    library.collections[0].item_count=2; /* Duplicate member 0. */
    assert(!ce_zotero_cache_export(&library,error,sizeof error) && *error);
    library.collections[0].item_count=1;
    library.collections[0].item_indices[0]=1;
    assert(!ce_zotero_cache_export(&library,error,sizeof error) && *error);
    library.collections[0].item_indices[0]=0;
    library.collections[0].library_id=0;
    assert(!ce_zotero_cache_export(&library,error,sizeof error) && *error);
    library.collections[0].library_id=INT64_MAX;
    library.collections[1].id=INT64_MIN;
    assert(!ce_zotero_cache_export(&library,error,sizeof error) && *error);
    library.collections[1].id=INT64_MAX;
    free(cache); ce_zotero_library_free(&library); ce_zotero_library_free(&restored);
}

/* Deliberately incompatible with free(payload): cJSON must release its own
 * buffers through the hook before any library/API result is returned. */
typedef union { max_align_t alignment; size_t tag; } CacheHookHeader;
static size_t cache_hook_live;
static void *cache_hook_allocate(size_t size) {
    if (size>SIZE_MAX-sizeof(CacheHookHeader)) return NULL;
    CacheHookHeader *header=malloc(sizeof *header+size); if (!header) return NULL;
    header->tag=0xC501; ++cache_hook_live; return header+1;
}
static void cache_hook_release(void *memory) {
    if (!memory) return;
    CacheHookHeader *header=(CacheHookHeader *)memory-1;
    assert(header->tag==0xC501 && cache_hook_live); header->tag=0; --cache_hook_live; free(header);
}
static void test_cache_allocator_contract(const char *path) {
    sqlite3 *database=create_db(path); modern_fixture(database); assert(sqlite3_close(database)==SQLITE_OK);
    size_t size; unsigned char *bytes=snapshot(path,&size);
    cJSON_Hooks hooks={cache_hook_allocate,cache_hook_release}; cJSON_InitHooks(&hooks);
    CeZoteroLibrary library={0}, restored={0}; char error[256];
    assert(ce_zotero_import_csl("[{\"id\":\"hooks\",\"title\":\"中文引用\"}]",&library,error,sizeof error));
    assert(!cache_hook_live);
    char *cache=ce_zotero_cache_export(&library,error,sizeof error); assert(cache && !cache_hook_live);
    assert(ce_zotero_cache_import(cache,&restored,error,sizeof error) && !cache_hook_live);
    free(cache); ce_zotero_library_free(&restored);
    assert(ce_zotero_load_sqlite(path,&library,error,sizeof error) && !cache_hook_live);
    cache=ce_zotero_cache_export(&library,error,sizeof error); assert(cache && !cache_hook_live);
    assert(ce_zotero_cache_import(cache,&restored,error,sizeof error) && !cache_hook_live);
    assert(restored.item_count==3 && restored.collection_count==8);
    free(cache); ce_zotero_library_free(&restored);
    assert(ce_zotero_load_sqlite_bytes(bytes,size,&library,error,sizeof error) && !cache_hook_live);
    ce_zotero_library_free(&library); free(bytes);
    cJSON_InitHooks(NULL); remove_path(path);
}

static int run_tests(const char *directory) {
    char path[2048], unicode_path[2048];
    assert(snprintf(path, sizeof path, "%s/test-zotero.sqlite", directory) < (int)sizeof path);
    assert(snprintf(unicode_path, sizeof unicode_path, "%s/test-zotero-中文.sqlite", directory) < (int)sizeof unicode_path);
    test_csl();
    test_cache_integer_precision();
    test_cache_allocator_contract(path);
    test_sqlite(path);
    test_metadata(path);
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
