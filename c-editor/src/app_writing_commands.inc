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

/* Only a typed space triggers the rule. Pasting Markdown or committing an IME
 * candidate remains literal text. The entire conversion is one undo step. */
static bool writing_markdown_input(App *a, const SDL_Event *event) {
    if (event->type!=SDL_EVENT_TEXT_INPUT || !event->text.text || strcmp(event->text.text," ") ||
        !a->active || !a->edit->has_focus || a->edit->composition_length || a->edit->composition_snapshot)
        return false;
    size_t start,end; get_selection(a,&start,&end);
    CeBlockInfo block;
    if (start!=end || !ce_document_block_at(a->doc,start,&block) ||
        block.kind!=CE_BLOCK_PARAGRAPH || block.depth || start<=block.start || start-block.start>6)
        return false;
    const char *text=ce_document_text(a->doc);
    for (size_t i=block.start;i<start;++i) if (text[i]!='#') return false;
    unsigned level=(unsigned)(start-block.start);
    CeDocument *candidate=edit_candidate(a);
    bool changed=candidate && ce_document_replace(candidate,block.start,start,"") &&
        ce_document_set_block(candidate,block.start,block.start,CE_BLOCK_HEADING,level);
    if (!edit_commit(a,candidate,changed,NULL,false)) { message(a,a->edit_error); return true; }
    sync_view(a); a->edit->cursor=(int)block.start; a->typing_mask=0;
    mark_changed(a); reveal_cursor(a);
    message(a,"已转换为标题 · 撤销可还原 # 标记");
    return true;
}
