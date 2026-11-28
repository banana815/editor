/* Writing commands share the same candidate/commit boundary as rich edits.
 * Formatting must not drop the selection or enter IME preedit into history. */
static bool writing_set_block(App *a, CeBlockKind kind, unsigned level) {
    size_t start, end; get_selection(a, &start, &end);
    int cursor=a->edit->cursor, selection1=a->edit->highlight1, selection2=a->edit->highlight2;
    if (!edit_block(a,start,end,kind,level)) { message(a,a->edit_error); return false; }
    sync_view(a);
    a->edit->cursor=cursor; a->edit->highlight1=selection1; a->edit->highlight2=selection2;
    a->typing_mask=0;
    mark_changed(a); reveal_cursor(a);
    return true;
}

static void writing_toggle_list(App *a, CeBlockKind kind) {
    size_t start,end; get_selection(a,&start,&end);
    bool same=true, found=false;
    for (size_t i=0;i<ce_document_block_count(a->doc);++i) {
        CeBlockInfo block;
        if (!ce_document_block_info(a->doc,i,&block)) continue;
        bool selected=start==end ? start>=block.start && start<=block.end :
            block.start<end && block.end>=start;
        if (!selected) continue;
        found=true;
        if (block.kind!=kind) same=false;
    }
    writing_set_block(a,found && same ? CE_BLOCK_PARAGRAPH : kind,0);
}

static bool writing_heading_shortcut(App *a, const SDL_Event *event) {
    if (event->type!=SDL_EVENT_KEY_DOWN || !a->active ||
        a->edit->composition_length || a->edit->composition_snapshot ||
        !(event->key.mod&(SDL_KMOD_CTRL|SDL_KMOD_GUI)) || !(event->key.mod&SDL_KMOD_ALT) ||
        event->key.key<SDLK_0 || event->key.key>SDLK_6) return false;
    unsigned level=(unsigned)(event->key.key-SDLK_0);
    if (writing_set_block(a,level ? CE_BLOCK_HEADING : CE_BLOCK_PARAGRAPH,level)) {
        char status[128];
        if (level) SDL_snprintf(status,sizeof status,"已设置 H%u 标题",level);
        else SDL_strlcpy(status,"已设置为正文",sizeof status);
        message(a,status);
    }
    return true;
}

/* Rules are entered through trusted text/Return events, never clipboard or
 * IME preedit. Candidate commit keeps marker removal and conversion one undo. */
static bool writing_convert(App *a,CeBlockInfo block,size_t end,CeBlockKind kind,
                            unsigned level,const char *language,unsigned number) {
    CeDocument *candidate=edit_candidate(a);
    bool changed=candidate && ce_document_replace(candidate,block.start,end,"");
    if(changed) changed=kind==CE_BLOCK_HEADING ? ce_document_set_block(candidate,block.start,block.start,kind,level) :
        ce_document_set_writing_block(candidate,block.start,kind,language,number);
    size_t caret=block.start;
    if(changed && kind==CE_BLOCK_RULE) { caret+=3; changed=ce_document_insert(candidate,caret,"\n"); caret++; }
    if(!edit_commit(a,candidate,changed,NULL,false)) { message(a,a->edit_error); return true; }
    sync_view(a); a->edit->cursor=(int)caret; a->typing_mask=0;
    mark_changed(a); reveal_cursor(a);
    message(a,kind==CE_BLOCK_CODE?"代码块 · Ctrl/Cmd+Enter 返回正文":"已转换格式 · 撤销可还原输入标记");
    return true;
}
static bool writing_markdown_input(App *a,const SDL_Event *event) {
    if(event->type!=SDL_EVENT_TEXT_INPUT || !event->text.text || strcmp(event->text.text," ") ||
       !a->active || !a->edit->has_focus || a->edit->composition_length || a->edit->composition_snapshot) return false;
    size_t start,end; get_selection(a,&start,&end); CeBlockInfo block;
    if(start!=end || !ce_document_block_at(a->doc,start,&block) || block.kind!=CE_BLOCK_PARAGRAPH ||
       block.depth || start<=block.start || start-block.start>10) return false;
    char marker[12]; size_t n=start-block.start; memcpy(marker,ce_document_text(a->doc)+block.start,n); marker[n]=0;
    if(n<=6 && strspn(marker,"#")==n) return writing_convert(a,block,start,CE_BLOCK_HEADING,(unsigned)n,NULL,0);
    if(n==1 && strchr("-*+",marker[0])) return writing_convert(a,block,start,CE_BLOCK_BULLET,0,NULL,0);
    if(!strcmp(marker,">")) return writing_convert(a,block,start,CE_BLOCK_QUOTE,0,NULL,0);
    if(n>=2 && n<=10 && (marker[n-1]=='.' || marker[n-1]==')') && strspn(marker,"0123456789")==n-1) {
        unsigned number=(unsigned)strtoul(marker,NULL,10);
        return writing_convert(a,block,start,CE_BLOCK_ORDERED,0,NULL,number);
    }
    return false;
}
static bool writing_return_event(App *a,const SDL_Event *event) {
    if(event->type!=SDL_EVENT_KEY_DOWN || (event->key.key!=SDLK_RETURN && event->key.key!=SDLK_KP_ENTER) ||
       !a->active || !a->edit->has_focus || a->edit->composition_length || a->edit->composition_snapshot ||
       (event->key.mod&(SDL_KMOD_SHIFT|SDL_KMOD_ALT))) return false;
    size_t start,end; get_selection(a,&start,&end); CeBlockInfo block;
    if(!ce_document_block_at(a->doc,start,&block)) return false;
    bool control=(event->key.mod&(SDL_KMOD_CTRL|SDL_KMOD_GUI))!=0;
    if(!control && start==end && block.start==block.end &&
       (block.kind==CE_BLOCK_BULLET || block.kind==CE_BLOCK_ORDERED)) {
        /* Lifting an empty item changes structure without inserting an LF.
         * The ordinary EditBox predicts +1 and would jump into the next block. */
        size_t old_length=ce_document_length(a->doc),caret=start;
        CeDocument *candidate=edit_candidate(a);
        bool ok=candidate && ce_document_insert(candidate,start,"\n");
        if(ok && ce_document_length(candidate)>old_length) caret+=ce_document_length(candidate)-old_length;
        if(!edit_commit(a,candidate,ok,NULL,false)) { message(a,a->edit_error); return true; }
        sync_view(a); a->edit->cursor=(int)caret; a->typing_mask=0; mark_changed(a); reveal_cursor(a); return true;
    }
    if(block.kind==CE_BLOCK_CODE && !control && start==end && end==block.end) {
        const char *text=ce_document_text(a->doc); size_t line=start;
        while(line>block.start && text[line-1]!='\n') line--;
        if(start-line==3 && !memcmp(text+line,"```",3)) {
            CeDocument *candidate=edit_candidate(a); size_t caret=0;
            size_t from=line>block.start?line-1:line;
            bool ok=candidate && ce_document_replace(candidate,from,end,"") &&
                ce_document_exit_writing_block(candidate,block.start,&caret);
            if(!edit_commit(a,candidate,ok,NULL,false)) { message(a,a->edit_error); return true; }
            sync_view(a); a->edit->cursor=(int)caret; a->typing_mask=0; mark_changed(a); reveal_cursor(a); return true;
        }
    }
    if(block.kind==CE_BLOCK_CODE || (block.kind==CE_BLOCK_QUOTE &&
       (control || (start==end && block.start==block.end)))) {
        CeDocument *candidate=edit_candidate(a); size_t caret=start+1;
        bool exit=control || block.kind==CE_BLOCK_QUOTE;
        bool ok=candidate && (exit ? ce_document_exit_writing_block(candidate,start,&caret) :
            ce_document_replace(candidate,start,end,"\n"));
        if(!edit_commit(a,candidate,ok,NULL,false)) { message(a,a->edit_error); return true; }
        sync_view(a); a->edit->cursor=(int)caret; a->typing_mask=0; mark_changed(a); reveal_cursor(a); return true;
    }
    if(control || start!=end || start!=block.end || block.kind!=CE_BLOCK_PARAGRAPH || block.depth) return false;
    size_t n=start-block.start; if(n<3 || n>35) return false;
    char marker[36]; memcpy(marker,ce_document_text(a->doc)+block.start,n); marker[n]=0;
    if(!strcmp(marker,"---") || !strcmp(marker,"***") || !strcmp(marker,"___"))
        return writing_convert(a,block,start,CE_BLOCK_RULE,0,NULL,0);
    if(!strncmp(marker,"```",3)) {
        for(size_t i=3;i<n;i++) if(!((marker[i]>='a' && marker[i]<='z') || (marker[i]>='A' && marker[i]<='Z') ||
            (marker[i]>='0' && marker[i]<='9') || strchr("_+-",marker[i]))) return false;
        return writing_convert(a,block,start,CE_BLOCK_CODE,0,marker+3,0);
    }
    return false;
}
