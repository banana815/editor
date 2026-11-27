/* Article organization is a sidecar workspace. History and trash retain exact
 * source bytes; previewing never touches the active model or its savepoint. */
enum { MAN_FOLDER, MAN_TAGS, MAN_PIN, MAN_STATUS, MAN_GOAL, MAN_PARAGRAPH,
       MAN_HISTORY, MAN_TRASH, MAN_NEW_FOLDER, MAN_FOLDERS, MAN_TAG_FILTER,
       MAN_STATUS_FILTER, MAN_CLEAR_FILTER, MAN_RESTORE };
static bool manager_ready(App *a) {
    if(a->writing_store.ready) return true;
    if(ce_writing_store_open(&a->writing_store,a->notes_root)) return true;
    message(a,a->writing_store.error); return false;
}
static void manager_load_article(App *a) {
    memset(&a->article_info,0,sizeof a->article_info); a->history_at=0;
    if(a->active && manager_ready(a) && !ce_writing_article_get(&a->writing_store,a->active,&a->article_info)) message(a,a->writing_store.error);
}
static bool manager_filter_files(App *a) {
    size_t *next=calloc(a->files.count?a->files.count:1,sizeof *next);
    if(!next) { message(a,"内存不足，文章列表保持不变"); return false; }
    bool ready=manager_ready(a); size_t count=0;
    /* Stable pinned-first order, then the store's portable filename order. */
    for(unsigned pass=0;pass<2;pass++) for(size_t i=0;i<a->files.count;i++) {
        CeArticleInfo info={0};
        if(ready) ce_writing_article_get(&a->writing_store,a->files.names[i],&info);
        if(info.pinned!=(pass==0)) continue;
        if(*a->folder_filter && strcmp(info.folder,a->folder_filter)) continue;
        if(*a->tag_filter && !strstr(info.tags,a->tag_filter)) continue;
        if(a->status_filter && info.status!=(unsigned)a->status_filter-1) continue;
        next[count++]=i;
    }
    free(a->visible_files); a->visible_files=next; a->visible_count=count; a->sidebar_scroll=0; return true;
}
static bool manager_checkpoint(App *a,bool force) {
    if(!a->active) return true;
    if(!force && a->history_at && SDL_GetTicks()-a->history_at<60000) return true;
    if(!manager_ready(a)) return false;
    char *data=NULL; size_t length=0;
    if(note_store_read(&a->store,a->active,&data,&length)!=NOTE_OK) { message(a,a->store.error); return false; }
    bool ok=!memchr(data,0,length) && ce_writing_snapshot(&a->writing_store,a->active,data,(int64_t)time(NULL));
    note_store_free(data);
    if(!ok) { message(a,a->writing_store.error); return false; }
    a->history_at=SDL_GetTicks()?SDL_GetTicks():1; return true;
}
static void manager_preview_clear(App *a) {
    free(a->manager_preview_text); free(a->manager_archive_data); free(a->manager_archive_name);
    a->manager_preview_text=a->manager_archive_data=a->manager_archive_name=NULL;
    a->manager_selected=-1; a->manager_preview_scroll=0;
}
static void manager_close(App *a) {
    a->manager_open=false; a->manager_pointer=false;
    EditBox_SetFocus(a->edit,a->active && !a->library_open && !a->search_open);
}
static void manager_show(App *a,int mode) {
    if(!manager_ready(a)) return;
    if(mode==1 && !a->active) { message(a,"请先打开文章查看历史版本"); return; }
    if(a->edit->composition_length) { message(a,"请先完成输入法选词"); return; }
    if(a->library_open) library_close(a);
    if(a->search_open) search_close(a);
    editing_break(a); a->manager_pointer=false; a->manager_mode=mode; a->manager_scroll=0; manager_preview_clear(a);
    if((mode==1 || mode==2) && !ce_writing_archive_list(&a->writing_store,mode==2,mode==1?a->active:NULL,&a->manager_archives)) {
        message(a,a->writing_store.error); return;
    }
    a->manager_open=true; a->touch_menu_visible=false; EditBox_SetFocus(a->edit,false);
}
static bool manager_save_info(App *a) {
    if(!a->active || !manager_ready(a)) return false;
    if(!ce_writing_article_set(&a->writing_store,a->active,&a->article_info)) {
        message(a,a->writing_store.error); manager_load_article(a); return false;
    }
    manager_filter_files(a); platform_persist(structure_persisted,a); message(a,"文章属性已保存"); return true;
}
static void manager_select_archive(App *a,size_t index) {
    if(index>=a->manager_archives.count) return;
    char *name=NULL,*data=NULL,*preview=NULL,error[256];
    if(!ce_writing_archive_read(&a->writing_store,a->manager_mode==2,a->manager_archives.entries[index].id,&name,&data)) { message(a,a->writing_store.error); return; }
    CeDocument *doc=ce_document_new();
    bool ok=doc && ce_document_import(doc,data,CE_FORMAT_AUTO,error,sizeof error);
    if(ok) preview=copy_string(ce_document_text(doc));
    ce_document_free(doc);
    if(!ok || !preview) { free(name); free(data); message(a,"这个版本无法预览，原文件和当前文章均保留"); return; }
    manager_preview_clear(a); a->manager_selected=(int)index;
    a->manager_archive_name=name; a->manager_archive_data=data; a->manager_preview_text=preview;
}
static void manager_restore(App *a,const char *name) {
    if(a->manager_selected<0 || !a->manager_archive_data) { message(a,"请先选择并预览一个版本"); return; }
    if(!save_document(a)) return;
    if(a->manager_mode==2) {
        if(!ce_writing_trash_restore(&a->writing_store,&a->store,
            a->manager_archives.entries[a->manager_selected].id,name)) { message(a,a->writing_store.error); return; }
        refresh_files(a); manager_close(a); open_note(a,name); platform_persist(structure_persisted,a);
        message(a,"文章已恢复，回收站副本继续保留"); return;
    }
    if(!manager_checkpoint(a,true)) return;
    CeDocument *candidate=ce_document_new();
    bool ok=candidate && ce_document_import(candidate,a->manager_archive_data,CE_FORMAT_AUTO,a->edit_error,sizeof a->edit_error);
    if(!edit_commit(a,candidate,ok,NULL,false)) { message(a,a->edit_error); return; }
    manager_close(a); sync_view(a); mark_changed(a); reveal_cursor(a);
    message(a,"历史版本已恢复 · 可撤销 · 恢复前内容已留存");
}
static bool manager_prompt_accept(App *a,const char *value) {
    Prompt mode=a->prompt_mode;
    if(mode<PROMPT_FOLDER || mode>PROMPT_RESTORE_NAME) return false;
    if(mode==PROMPT_RESTORE_NAME) {
        char *name=NULL;
        if(note_store_normalize_name(value,&name)!=NOTE_OK) { message(a,"请输入有效文件名；同名文章不会被覆盖"); return true; }
        manager_restore(a,name); note_store_free(name);
        if(!a->manager_open) close_prompt(a);
        return true;
    }
    if(mode==PROMPT_NEW_FOLDER) {
        if(!ce_writing_folder_add(&a->writing_store,value)) { message(a,a->writing_store.error); return true; }
        platform_persist(structure_persisted,a);
    } else if(mode==PROMPT_TAG_FILTER) {
        if(strlen(value)>=sizeof a->tag_filter) { message(a,"筛选文字过长"); return true; }
        SDL_strlcpy(a->tag_filter,value,sizeof a->tag_filter); manager_filter_files(a);
    } else {
        CeArticleInfo next=a->article_info;
        if(mode==PROMPT_FOLDER || mode==PROMPT_TAGS) {
            char *target=mode==PROMPT_FOLDER?next.folder:next.tags;
            size_t size=mode==PROMPT_FOLDER?sizeof next.folder:sizeof next.tags;
            if(strlen(value)>=size) { message(a,"输入过长，请缩短后重试"); return true; }
            SDL_strlcpy(target,value,size);
        } else {
            char *end=NULL; unsigned long goal=strtoul(value,&end,10);
            if(!*value || *value=='-' || !end || *end || goal>10000000) { message(a,"请输入 0 至 10000000 的整数；0 表示关闭目标"); return true; }
            next.goal=(size_t)goal;
        }
        a->article_info=next;
        if(!manager_save_info(a)) return true;
    }
    close_prompt(a); EditBox_SetFocus(a->edit,false); return true;
}
static void manager_prompt(App *a,Prompt mode,const char *value) {
    show_prompt(a,mode); EditBox_SetText(a->prompt,value); a->prompt->cursor=(int)strlen(value);
}
static void manager_action(App *a,int action) {
    if(action==MAN_HISTORY) { manager_show(a,1); return; }
    if(action==MAN_TRASH) { manager_show(a,2); return; }
    if(action==MAN_FOLDERS) { manager_show(a,3); return; }
    if(action==MAN_NEW_FOLDER) { manager_prompt(a,PROMPT_NEW_FOLDER,""); return; }
    if(action==MAN_TAG_FILTER) { manager_prompt(a,PROMPT_TAG_FILTER,a->tag_filter); return; }
    if(action==MAN_STATUS_FILTER) { a->status_filter=(a->status_filter+1)%4; manager_filter_files(a); return; }
    if(action==MAN_CLEAR_FILTER) { *a->folder_filter=*a->tag_filter=0; a->status_filter=0; manager_filter_files(a); return; }
    if(action==MAN_PARAGRAPH) { a->paragraph_focus=!a->paragraph_focus; return; }
    if(action==MAN_RESTORE) {
        if(a->manager_selected<0) { message(a,"请选择左侧版本进行预览"); return; }
        if(a->manager_mode==2) manager_prompt(a,PROMPT_RESTORE_NAME,a->manager_archive_name);
        else manager_restore(a,NULL);
        return;
    }
    if(!a->active) { message(a,"请先打开文章设置属性"); return; }
    if(action==MAN_FOLDER) manager_prompt(a,PROMPT_FOLDER,a->article_info.folder);
    else if(action==MAN_TAGS) manager_prompt(a,PROMPT_TAGS,a->article_info.tags);
    else if(action==MAN_GOAL) { char value[32]; SDL_snprintf(value,sizeof value,"%zu",a->article_info.goal); manager_prompt(a,PROMPT_GOAL,value); }
    else if(action==MAN_PIN) { a->article_info.pinned=!a->article_info.pinned; manager_save_info(a); }
    else if(action==MAN_STATUS) { a->article_info.status=(a->article_info.status+1)%3; manager_save_info(a); }
}
static void manager_layout(App *a) {
    float width=SDL_min(900.0f,a->width-16.0f),height=SDL_max(80.0f,a->height-64.0f);
    a->manager_area=(SDL_FRect){(a->width-width)/2,8,width,height};
    float x=a->manager_area.x;
    a->manager_close=(SDL_FRect){x+width-80,18,68,36};
    for(size_t i=0;i<SDL_arraysize(a->manager_buttons);i++) a->manager_buttons[i]=(SDL_FRect){0};
    if(!a->manager_mode) {
        float cell=(width-36)/2;
        a->manager_scroll=SDL_clamp(a->manager_scroll,0,SDL_max(0,7*60-height+86));
        for(size_t i=0;i<13;i++) a->manager_buttons[i]=(SDL_FRect){x+12+(i%2)*(cell+12),84+(i/2)*60-a->manager_scroll,cell,52};
    } else {
        a->manager_buttons[MAN_RESTORE]=(SDL_FRect){x+12,18,116,36};
        if(a->manager_mode==3) a->manager_buttons[MAN_RESTORE].w=0;
        bool narrow=width<600;
        a->manager_rows=(SDL_FRect){x+12,70,narrow?width-24:270,narrow?160:height-76};
        a->manager_preview=narrow?(SDL_FRect){x+12,246,width-24,SDL_max(20,height-252)}:
            (SDL_FRect){x+300,70,width-312,height-76};
        size_t rows=a->manager_mode==3?ce_writing_folder_count(&a->writing_store)+1:a->manager_archives.count;
        a->manager_scroll=SDL_clamp(a->manager_scroll,0,SDL_max(0,rows*64.0f-a->manager_rows.h));
    }
}
static void manager_draw(App *a) {
    if(!a->manager_open) return;
    manager_layout(a);
    SDL_Color text=ink(a),surface=a->dark?(SDL_Color){44,51,58,255}:(SDL_Color){237,242,238,255};
    SDL_SetRenderDrawBlendMode(a->renderer,SDL_BLENDMODE_BLEND);
    fill(a,(SDL_FRect){0,0,a->width,a->height},(SDL_Color){0,0,0,100}); SDL_SetRenderDrawBlendMode(a->renderer,SDL_BLENDMODE_NONE);
    fill(a,a->manager_area,paper(a)); fill(a,a->manager_close,surface); label(a,"关闭",a->manager_close.x+16,25,text);
    const char *titles[]={"文章管理","历史版本 · 选择后预览","回收站 · 恢复为新文件","文件夹 · 点击筛选文章"};
    float title_x=a->manager_area.x+(a->manager_mode==1 || a->manager_mode==2?140:12);
    clipped_label(a,titles[a->manager_mode],(SDL_FRect){title_x,25,a->manager_close.x-title_x-8,28},text);
    if(!a->manager_mode) {
        const char *states[]={"草稿","待修改","已发布"};
        char names[13][768];
        SDL_snprintf(names[0],sizeof names[0],"文件夹：%s",*a->article_info.folder?a->article_info.folder:"未分类");
        SDL_snprintf(names[1],sizeof names[1],"标签：%s",*a->article_info.tags?a->article_info.tags:"未设置");
        SDL_snprintf(names[2],sizeof names[2],"%s置顶文章",a->article_info.pinned?"取消":"");
        SDL_snprintf(names[3],sizeof names[3],"状态：%s",states[a->article_info.status]);
        SDL_snprintf(names[4],sizeof names[4],"目标：%zu 字",a->article_info.goal);
        SDL_snprintf(names[5],sizeof names[5],"段落聚焦：%s",a->paragraph_focus?"开启":"关闭");
        SDL_strlcpy(names[6],"历史版本",sizeof names[6]); SDL_strlcpy(names[7],"回收站",sizeof names[7]);
        SDL_strlcpy(names[8],"新建文件夹",sizeof names[8]); SDL_strlcpy(names[9],"按文件夹筛选",sizeof names[9]);
        SDL_snprintf(names[10],sizeof names[10],"标签筛选：%s",*a->tag_filter?a->tag_filter:"全部");
        SDL_snprintf(names[11],sizeof names[11],"状态筛选：%s",a->status_filter?states[a->status_filter-1]:"全部");
        SDL_strlcpy(names[12],"清除文章筛选",sizeof names[12]);
        SDL_Rect clip={(int)a->manager_area.x+8,76,(int)a->manager_area.w-16,(int)a->manager_area.h-72}; SDL_SetRenderClipRect(a->renderer,&clip);
        for(size_t i=0;i<13;i++) { SDL_FRect box=a->manager_buttons[i]; fill(a,box,surface); clipped_label(a,names[i],(SDL_FRect){box.x+8,box.y+12,box.w-16,30},text); }
        SDL_SetRenderClipRect(a->renderer,NULL);
    } else {
        if(a->manager_buttons[MAN_RESTORE].w) { fill(a,a->manager_buttons[MAN_RESTORE],surface); label(a,"恢复此版本",a->manager_buttons[MAN_RESTORE].x+9,25,text); }
        SDL_Rect clip={(int)a->manager_rows.x,(int)a->manager_rows.y,(int)a->manager_rows.w,(int)a->manager_rows.h}; SDL_SetRenderClipRect(a->renderer,&clip);
        size_t count=a->manager_mode==3?ce_writing_folder_count(&a->writing_store)+1:a->manager_archives.count;
        if(!count) label(a,"暂无历史记录",clip.x+8,clip.y+16,text);
        for(size_t i=0;i<count;i++) {
            float y=clip.y+i*64.0f-a->manager_scroll; if(y+60<clip.y || y>clip.y+clip.h) continue;
            fill(a,(SDL_FRect){clip.x,y,clip.w,60},(int)i==a->manager_selected?(SDL_Color){163,193,176,255}:surface);
            if(a->manager_mode==3) clipped_label(a,i?ce_writing_folder_at(&a->writing_store,i-1):"全部文件夹",(SDL_FRect){clip.x+8,y+14,clip.w-16,30},text);
            else {
                CeArchiveEntry *entry=&a->manager_archives.entries[i]; char stamp[64];
                time_t seconds=(time_t)entry->timestamp; struct tm *local=localtime(&seconds);
                if(local) strftime(stamp,sizeof stamp,"%Y-%m-%d %H:%M:%S",local); else SDL_strlcpy(stamp,"历史版本",sizeof stamp);
                clipped_label(a,stamp,(SDL_FRect){clip.x+8,y+3,clip.w-16,26},text);
                clipped_label(a,entry->name,(SDL_FRect){clip.x+8,y+30,clip.w-16,26},text);
            }
        }
        SDL_SetRenderClipRect(a->renderer,NULL);
        if(a->manager_mode!=3) {
            SDL_FRect box=a->manager_preview; fill(a,box,surface);
            TTF_Text *preview=TTF_CreateText(a->engine,a->ui_font,a->manager_preview_text?a->manager_preview_text:"选择一个版本预览。\n恢复前会保留当前文章，回收站副本不会自动删除。",0);
            if(preview) {
                TTF_SetTextWrapWidth(preview,(int)SDL_max(1,box.w-20)); TTF_SetTextColor(preview,text.r,text.g,text.b,255);
                int w,h; TTF_GetTextSize(preview,&w,&h); a->manager_preview_height=(float)h;
                a->manager_preview_scroll=SDL_clamp(a->manager_preview_scroll,0,SDL_max(0,h-box.h+20));
                SDL_Rect area={(int)box.x,(int)box.y,(int)box.w,(int)box.h}; SDL_SetRenderClipRect(a->renderer,&area);
                TTF_DrawRendererText(preview,box.x+10,box.y+10-a->manager_preview_scroll); TTF_DestroyText(preview); SDL_SetRenderClipRect(a->renderer,NULL);
            }
        }
    }
}
static bool manager_event(App *a,SDL_Event *event) {
    if(!a->manager_open) return false;
    manager_layout(a);
    if(event->type==SDL_EVENT_WINDOW_FOCUS_LOST) a->manager_pointer=false;
    if(event->type==SDL_EVENT_KEY_DOWN && event->key.key==SDLK_ESCAPE) manager_close(a);
    else if(event->type==SDL_EVENT_MOUSE_WHEEL) {
        if(a->manager_mode && contains(a->manager_preview,event->wheel.mouse_x,event->wheel.mouse_y)) a->manager_preview_scroll-=event->wheel.y*40;
        else a->manager_scroll-=event->wheel.y*40;
        manager_layout(a);
    } else if(event->type==SDL_EVENT_MOUSE_MOTION && a->manager_pointer) {
        float y=event->motion.y;
        if(SDL_fabsf(y-a->manager_pointer_y)>8 || SDL_fabsf(event->motion.x-a->manager_pointer_x)>8) a->manager_moved=true;
        if(a->manager_moved) {
            if(a->manager_preview_drag) a->manager_preview_scroll=SDL_clamp(a->manager_preview_scroll+a->manager_pointer_last_y-y,0,SDL_max(0,a->manager_preview_height-a->manager_preview.h+20));
            else a->manager_scroll+=a->manager_pointer_last_y-y;
            manager_layout(a);
        }
        a->manager_pointer_last_y=y;
    } else if(event->type==SDL_EVENT_MOUSE_BUTTON_DOWN && event->button.button==SDL_BUTTON_LEFT) {
        a->manager_pointer=true; a->manager_moved=false;
        a->manager_pointer_x=event->button.x; a->manager_pointer_y=a->manager_pointer_last_y=event->button.y;
        a->manager_preview_drag=a->manager_mode && contains(a->manager_preview,event->button.x,event->button.y);
    } else if(event->type==SDL_EVENT_MOUSE_BUTTON_UP && a->manager_pointer) {
        a->manager_pointer=false;
        if(a->manager_moved) return true;
        float x=event->button.x,y=event->button.y;
        if(contains(a->manager_close,x,y)) manager_close(a);
        else if(!a->manager_mode && y>=76 && y<a->manager_area.y+a->manager_area.h) {
            for(size_t i=0;i<13;i++) if(contains(a->manager_buttons[i],x,y)) { manager_action(a,(int)i); break; }
        } else if(contains(a->manager_buttons[MAN_RESTORE],x,y)) manager_action(a,MAN_RESTORE);
        else if(a->manager_mode && contains(a->manager_rows,x,y)) {
            size_t index=(size_t)((y-a->manager_rows.y+a->manager_scroll)/64);
            if(a->manager_mode==3 && index<=ce_writing_folder_count(&a->writing_store)) {
                SDL_strlcpy(a->folder_filter,index?ce_writing_folder_at(&a->writing_store,index-1):"",sizeof a->folder_filter);
                manager_filter_files(a); a->outline=false; a->sidebar=true; a->sidebar_manual=true; manager_close(a);
            } else manager_select_archive(a,index);
        }
    }
    return true;
}
