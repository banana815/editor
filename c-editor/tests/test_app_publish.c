#define SDL_MAIN_HANDLED 1
#include "../src/app.c"
#undef main
#include <assert.h>

static void unchanged(App *a,const char *before) {
    char *after=ce_document_export_json(a->doc); assert(after && !strcmp(before,after)); free(after);
}
int main(int argc,char **argv) {
    assert(argc>=3); void *state=NULL;
    char *args[]={"publish-test","--notes",argv[1],"--font",argv[2]};
    assert(SDL_AppInit(&state,5,args)==SDL_APP_CONTINUE); App *a=state;
    char error[512];
    assert(ce_document_import(a->doc,"A draft & 中文",CE_FORMAT_TEXT,error,sizeof error));
    free(a->active); a->active=copy_string("文章.json"); assert(a->active); sync_view(a);
    char *before=ce_document_export_json(a->doc),*md=publish_prepare(a,CE_PUBLISH_MARKDOWN);
    assert(md && strstr(md,"A draft &amp; 中文")); free(md); unchanged(a,before); free(before);
    a->edit->composition_length=1;
    assert(!publish_prepare(a,CE_PUBLISH_HTML) && *a->edit_error); a->edit->composition_length=0;

    CeZoteroItem item={0}; item.id="A"; item.item_key="A"; item.title="Book";
    item.csl_json="{\"id\":\"A\",\"type\":\"book\",\"title\":\"An authoritative book\",\"author\":[{\"family\":\"Doe\",\"given\":\"Jane\"}],\"issued\":{\"date-parts\":[[2020]]},\"publisher\":\"Press\"}";
    char *id=NULL; assert(ce_citation_insert(a->doc,0,&item,"12","quoted <script>text</script>",&id,error,sizeof error)); free(id);
    char *json=ce_document_export_json(a->doc); cJSON *tree=cJSON_Parse(json); free(json); assert(tree);
    cJSON *node; cJSON_ArrayForEach(node,cJSON_GetObjectItemCaseSensitive(tree,"content")) {
        cJSON *type=cJSON_GetObjectItemCaseSensitive(node,"type");
        if(cJSON_IsString(type) && !strcmp(type->valuestring,"zoteroCitationBlock")) {
            cJSON *attrs=cJSON_GetObjectItemCaseSensitive(node,"attrs");
            cJSON_DeleteItemFromObjectCaseSensitive(attrs,"cslRenderJson");
            assert(cJSON_AddStringToObject(attrs,"cslRenderJson","POISON_CACHE<script>alert(1)</script>"));
        }
    }
    json=cJSON_PrintUnformatted(tree); cJSON_Delete(tree); assert(json);
    assert(ce_document_import(a->doc,json,CE_FORMAT_JSON,error,sizeof error)); free(json); sync_view(a);
    before=ce_document_export_json(a->doc);
    char *html=publish_prepare(a,CE_PUBLISH_HTML);
    if(!html) fprintf(stderr,"%s\n",a->edit_error);
    assert(html && strstr(html,"Doe") && strstr(html,"2020") && strstr(html,"An authoritative book"));
    assert(strstr(html,"quoted &lt;script&gt;text&lt;/script&gt;") && !strstr(html,"POISON_CACHE") && !strstr(html,"cslJson"));
    assert(strstr(html,"class=\"references\"")); free(html); unchanged(a,before); free(before);
    assert(!ce_document_can_undo(a->doc) && !ce_document_is_modified(a->doc));
    assert(ce_document_import(a->doc,"{\"type\":\"doc\",\"content\":[{\"type\":\"zoteroCitationBlock\",\"attrs\":{\"citationId\":\"missing\",\"cslRenderJson\":\"POISON\"}}]}",CE_FORMAT_JSON,error,sizeof error));
    before=ce_document_export_json(a->doc); assert(!publish_prepare(a,CE_PUBLISH_HTML) && *a->edit_error);
    unchanged(a,before); free(before);
    SDL_AppQuit(a,SDL_APP_SUCCESS); return 0;
}
