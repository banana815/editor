/* Private application controller. Included after the document transaction and
 * view helpers; all format interpretation and deletion decisions stay in C. */
struct EditorClipboardRequest {
    App *app;
    char *note, *prompt_snapshot, *plain, *html, *typed;
    uint64_t revision, prompt_generation;
    size_t start, end;
    bool cut, prompt;
    CeDocumentSlice *slice;
    CeDocumentCutGuard *guard;
};
static void clipboard_request_free(EditorClipboardRequest *r) {
    if(!r) return;
    free(r->note); free(r->prompt_snapshot); free(r->plain); free(r->html); free(r->typed);
    ce_document_slice_free(r->slice); ce_document_cut_guard_free(r->guard); free(r);
}
static bool clipboard_atom_text(void *context,const cJSON *atom,char **text,char *error,size_t capacity) {
    App *a=context; const CeCitationCache *cache=NULL;
    if(citation_resolve(a,atom,&cache,error,capacity)!=CE_RICH_CITATION_VALID ||
       ce_citation_cache_kind(cache)!=CE_CITATION_CACHE_CITATION) return true;
    CeCslOutputString result={0};
    if(!ce_csl_output_plain(ce_citation_cache_citation_output(cache),&result,error,capacity)) return false;
    char *copy=copy_string(result.data); ce_csl_output_string_free(&result);
    if(!copy) { SDL_strlcpy(error,"内存不足，未复制",capacity); return false; }
    *text=copy; return true;
}
static EditorClipboardRequest *clipboard_request(App *a,bool cut,bool capture) {
    EditBox *edit=clipboard_edit(a);
    if(!edit || (!a->active && edit==a->edit)) return NULL;
    EditorClipboardRequest *r=calloc(1,sizeof *r);
    if(!r) return NULL;
    r->app=a; r->revision=a->citation_revision; r->prompt_generation=a->prompt_generation;
    r->cut=cut; r->prompt=edit==a->prompt;
    if(edit->highlight1>=0 && edit->highlight2>=0) {
        r->start=(size_t)SDL_min(edit->highlight1,edit->highlight2);
        r->end=(size_t)SDL_max(edit->highlight1,edit->highlight2);
    } else r->start=r->end=(size_t)SDL_max(0,edit->cursor);
    if(r->prompt) {
        r->prompt_snapshot=copy_string(edit->text->text?edit->text->text:"");
        if(!r->prompt_snapshot) goto fail;
        if(capture) { r->plain=browser_clipboard_read(a); if(!r->plain) goto fail; }
    } else {
        r->note=copy_string(a->active);
        if(!r->note) goto fail;
        if(capture) {
            if(r->start==r->end || !ce_document_slice_capture(a->doc,r->start,r->end,&r->slice,
                cut?&r->guard:NULL,a->edit_error,sizeof a->edit_error) ||
               !ce_document_slice_json(r->slice,&r->typed,a->edit_error,sizeof a->edit_error) ||
               !ce_document_slice_plain(r->slice,clipboard_atom_text,a,&r->plain,a->edit_error,sizeof a->edit_error) ||
               !ce_document_slice_html(r->slice,clipboard_atom_text,a,&r->html,a->edit_error,sizeof a->edit_error)) goto fail;
        }
    }
    return r;
fail:
    clipboard_request_free(r); return NULL;
}
static bool clipboard_request_current(EditorClipboardRequest *r) {
    App *a=r->app; EditBox *edit=clipboard_edit(a); size_t start,end;
    if(!edit || r->prompt!=(edit==a->prompt) || a->citation_revision!=r->revision) return false;
    if(edit->highlight1>=0 && edit->highlight2>=0) {
        start=(size_t)SDL_min(edit->highlight1,edit->highlight2);
        end=(size_t)SDL_max(edit->highlight1,edit->highlight2);
    } else start=end=(size_t)SDL_max(0,edit->cursor);
    if(start!=r->start || end!=r->end) return false;
    if(r->prompt) return r->prompt_generation==a->prompt_generation &&
        !strcmp(r->prompt_snapshot,edit->text->text?edit->text->text:"");
    if(!a->active || strcmp(a->active,r->note)) return false;
    if(r->guard) {
        bool matches=false;
        return ce_document_cut_guard_matches(r->guard,a->doc,start,end,&matches,a->edit_error,sizeof a->edit_error) && matches;
    }
    return true;
}
static bool clipboard_apply(EditorClipboardRequest *r,const CeDocumentSlice *slice) {
    App *a=r->app; CeDocument *candidate=NULL; CeDocumentSliceAnchor anchor={0};
    if(!clipboard_request_current(r)) { message(a,"选区或笔记已改变，剪贴板操作已取消"); return false; }
    if(!ce_document_slice_prepare_replace(a->doc,r->start,r->end,slice,&candidate,&anchor,a->edit_error,sizeof a->edit_error)) {
        message(a,a->edit_error); return false;
    }
    size_t ordinal=0; bool anchored=false;
    for(size_t i=0;i<ce_document_block_count(candidate);++i) {
        CeBlockInfo block;
        if(!ce_document_block_info(candidate,i,&block) || block.kind==CE_BLOCK_ENDNOTES) continue;
        if(i==anchor.block_index) { anchored=true; break; }
        ++ordinal;
    }
    if(!edit_commit(a,candidate,true,NULL,false)) { message(a,a->edit_error); return false; }
    size_t cursor=anchor.offset;
    if(anchored) {
        size_t current=0;
        for(size_t i=0;i<ce_document_block_count(a->doc);++i) {
            CeBlockInfo block;
            if(!ce_document_block_info(a->doc,i,&block) || block.kind==CE_BLOCK_ENDNOTES) continue;
            if(current++==ordinal) { cursor=block.start+SDL_min(anchor.block_offset,block.end-block.start); break; }
        }
    }
    sync_view(a); a->edit->cursor=(int)SDL_min(cursor,ce_document_length(a->doc));
    mark_changed(a); reveal_cursor(a); return true;
}
static bool clipboard_cut_commit(EditorClipboardRequest *r) {
    if(!clipboard_request_current(r)) { message(r->app,"复制已完成；选区已改变，原文未删除"); return false; }
    if(r->prompt) return EditBox_DeleteHighlight(r->app->prompt);
    return clipboard_apply(r,NULL);
}
static bool clipboard_paste_data(EditorClipboardRequest *r,const char *plain,const char *html,const char *typed) {
    App *a=r->app;
    if(!clipboard_request_current(r)) { message(a,"选区或笔记已改变，粘贴已取消"); return false; }
    if(r->prompt) {
        if(!plain || strlen(plain)>CE_DOCUMENT_SLICE_MAX_BYTES) return false;
        SDL_Event event={0}; event.type=SDL_EVENT_TEXT_INPUT; event.text.text=plain;
        EditBox_HandleEvent(a->prompt,&event); return true;
    }
    CeDocumentSlice *slice=NULL; bool found=false,ok=false;
    if(typed && *typed) ok=ce_document_slice_parse(typed,strlen(typed),&slice,a->edit_error,sizeof a->edit_error);
    else {
        if(html && *html) {
            ok=ce_document_slice_from_html(html,strlen(html),&slice,&found,a->edit_error,sizeof a->edit_error);
            if(!ok) goto done;
        }
        if(!found) {
            const char *source=html && *html?html:plain;
            if(!source || strlen(source)>CE_DOCUMENT_SLICE_MAX_BYTES) {
                SDL_strlcpy(a->edit_error,"剪贴板内容为空或超过大小限制",sizeof a->edit_error); goto done;
            }
            CeDocument *temporary=ce_document_new();
            ok=temporary && ce_document_import(temporary,source,html && *html?CE_FORMAT_HTML:CE_FORMAT_TEXT,
                a->edit_error,sizeof a->edit_error) && ce_document_slice_capture(temporary,0,
                ce_document_length(temporary),&slice,NULL,a->edit_error,sizeof a->edit_error);
            ce_document_free(temporary);
        }
    }
    if(ok) ok=clipboard_apply(r,slice);
done:
    if(!ok) message(a,*a->edit_error?a->edit_error:"无法粘贴此内容，原文保持不变");
    ce_document_slice_free(slice); return ok;
}
static bool browser_clipboard_prepare(bool cut,PlatformWebClipboardData *data,void *state) {
    App *a=state; clipboard_request_free(a->clipboard_event); a->clipboard_event=NULL;
    a->edit_error[0]=0;
    EditorClipboardRequest *r=clipboard_request(a,cut,true);
    if(!r) { if(*a->edit_error) message(a,a->edit_error); return false; }
    a->clipboard_event=r; *data=(PlatformWebClipboardData){r->plain,r->html,r->typed}; return true;
}
static void browser_clipboard_finish(bool accepted,void *state) {
    App *a=state; EditorClipboardRequest *r=a->clipboard_event; a->clipboard_event=NULL;
    if(!r) return;
    if(accepted && r->cut) clipboard_cut_commit(r);
    else if(!accepted) browser_clipboard_error(a);
    clipboard_request_free(r);
}
static bool browser_clipboard_paste(const PlatformWebClipboardData *data,void *state) {
    App *a=state; EditorClipboardRequest *r=clipboard_request(a,false,false);
    if(!r) return false;
    a->edit_error[0]=0;
    bool ok=clipboard_paste_data(r,data->plain,data->html,data->typed_json);
    clipboard_request_free(r); return ok;
}
static void clipboard_write_complete(bool success,const char *plain,const char *html,const char *typed,const char *error,void *userdata) {
    (void)plain; (void)html; (void)typed;
    EditorClipboardRequest *r=userdata;
    if(success && r->cut) clipboard_cut_commit(r);
    else if(!success) message(r->app,error && *error?error:"无法写入剪贴板，原文保持不变");
    clipboard_request_free(r);
}
static void clipboard_read_complete(bool success,const char *plain,const char *html,const char *typed,const char *error,void *userdata) {
    EditorClipboardRequest *r=userdata;
    if(success) clipboard_paste_data(r,plain,html,typed);
    else message(r->app,error && *error?error:"无法读取剪贴板");
    clipboard_request_free(r);
}
#ifndef __EMSCRIPTEN__
typedef struct { char *plain,*html,*typed; unsigned references; bool released; } NativeClipboard;
static void native_clipboard_release(NativeClipboard *p) {
    if(!--p->references) { free(p->plain); free(p->html); free(p->typed); free(p); }
}
static void SDLCALL native_clipboard_cleanup(void *userdata) {
    NativeClipboard *p=userdata; p->released=true; native_clipboard_release(p);
}
static const void *SDLCALL native_clipboard_data(void *userdata,const char *mime,size_t *size) {
    NativeClipboard *p=userdata; const char *data=NULL;
    if(mime) data=!strcmp(mime,"text/plain")?p->plain:!strcmp(mime,"text/html")?p->html:
        !strcmp(mime,CE_DOCUMENT_SLICE_MIME)?p->typed:NULL;
    *size=data?strlen(data):0; return data;
}
static bool native_clipboard_write(EditorClipboardRequest *r) {
    NativeClipboard *p=calloc(1,sizeof *p);
    if(!p) return false;
    p->plain=copy_string(r->plain); p->html=r->html?copy_string(r->html):NULL;
    p->typed=r->typed?copy_string(r->typed):NULL; p->references=1;
    if(!p->plain || (r->html && !p->html) || (r->typed && !p->typed)) { native_clipboard_release(p); return false; }
    const char *formats[]={"text/plain","text/html",CE_DOCUMENT_SLICE_MIME};
    /* SDL can retain callbacks after an OS write failure. Keep a local reference
     * while clearing that failed offer, including synchronous cleanup paths. */
    ++p->references;
    bool ok=SDL_SetClipboardData(native_clipboard_data,native_clipboard_cleanup,p,formats,r->typed?3:1);
    if(!ok && !p->released) {
        SDL_ClearClipboardData();
        if(!p->released) native_clipboard_cleanup(p); /* video unavailable */
    }
    native_clipboard_release(p); return ok;
}
static char *native_clipboard_read(const char *mime,bool *ok) {
    if(!SDL_HasClipboardData(mime)) return NULL;
    size_t size=0; void *raw=SDL_GetClipboardData(mime,&size); char *copy=NULL;
    if(raw && size<=CE_DOCUMENT_SLICE_MAX_BYTES && !memchr(raw,0,size)) {
        copy=malloc(size+1); if(copy) { memcpy(copy,raw,size); copy[size]=0; }
    }
    SDL_free(raw); if(!copy) *ok=false; return copy;
}
#endif
static void editor_clipboard_action(App *a,CeEditorClipboardAction action) {
    EditBox *edit=clipboard_edit(a);
    if(!edit) return;
    if(action==CE_EDITOR_SELECT_ALL) {
        size_t size=strlen(edit->text->text?edit->text->text:"");
        edit->highlight1=0; edit->highlight2=edit->cursor=(int)size; return;
    }
    a->edit_error[0]=0;
    EditorClipboardRequest *r=clipboard_request(a,action==CE_EDITOR_CUT,action!=CE_EDITOR_PASTE);
    if(!r) { if(*a->edit_error) message(a,a->edit_error); return; }
    if(action==CE_EDITOR_PASTE) {
#ifdef __EMSCRIPTEN__
        if(platform_web_clipboard_read(clipboard_read_complete,r)) return;
        clipboard_read_complete(false,NULL,NULL,NULL,"浏览器未允许读取剪贴板",r);
#else
        bool ok=true; char *typed=native_clipboard_read(CE_DOCUMENT_SLICE_MIME,&ok);
        char *html=typed?NULL:native_clipboard_read("text/html",&ok);
        char *plain=native_clipboard_read("text/plain",&ok);
        clipboard_read_complete(ok,plain,html,typed,"无法读取剪贴板内容",r);
        free(typed); free(html); free(plain);
#endif
    } else {
#ifdef __EMSCRIPTEN__
        if(platform_web_clipboard_write(r->plain,r->html,r->typed,clipboard_write_complete,r)) return;
        clipboard_write_complete(false,NULL,NULL,NULL,"浏览器未允许写入剪贴板",r);
#else
        bool ok=native_clipboard_write(r);
        clipboard_write_complete(ok,NULL,NULL,NULL,"无法写入剪贴板，原文保持不变",r);
#endif
    }
}
