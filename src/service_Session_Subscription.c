/* This is a managed file. Do not delete this comment. */

#include <corto.ws>

void ws_service_Session_Subscription_addEvent(
    ws_service_Session_Subscription this,
    corto_event *e)
{
    ut_ll_append(this->batch, e);
}

int16_t ws_service_Session_Subscription_construct(
    ws_service_Session_Subscription this)
{
    corto_set_str(&corto_subscriber(this)->format, "binary/corto");
    return corto_subscriber_construct(this);
}

static
void ws_type_identifier(
    corto_type type,
    corto_id typeId)
{
    if (corto_check_attr(type, CORTO_ATTR_NAMED) && corto_childof(root_o, type)) {
        corto_fullpath(typeId, type);
    } else {
        long unsigned int len = sizeof(corto_id);

        /* base64 encode type address */
        base64_encode((unsigned char*)&type, sizeof(void*), (unsigned char*)typeId, &len);
    }
}

typedef struct ws_typeSerializer_t {
    ws_service_Session session;
    ws_data *msg;
    ws_dataType *dataType;
    ut_strbuf memberBuff;
    corto_int32 count;
} ws_typeSerializer_t;

static
ws_dataType* ws_data_addMetadata(
    ws_service_Session session,
    ws_data *msg,
    corto_type t);

static
int16_t ws_typeSerializer_member(
    corto_walk_opt* s,
    corto_value *info,
    void *userData)
{
    ws_typeSerializer_t *data = userData;
    corto_type type = corto_value_typeof(info);
    corto_member m = NULL;
    if (info->kind == CORTO_MEMBER) {
        m = info->is.member.member;
    }

    if (data->count) {
        ut_strbuf_appendstr(&data->memberBuff, ",");
    } else {
        ut_strbuf_appendstr(&data->memberBuff, "[");
    }

    corto_id typeId;
    if (type->reference) {
        type = corto_object_o;
    }

    ws_type_identifier(type, typeId);

    corto_string escaped = ws_serializer_escape(typeId, NULL);
    ut_strbuf_append(
        &data->memberBuff,
        "[\"%s\",\"%s\"",
        m ? corto_idof(m) : "super",
        escaped);

    /* If member contains additional meta information, serialize it */
    bool add_modifiers = m
            ? ((m->modifiers &
                (CORTO_READONLY|CORTO_CONST|CORTO_KEY|CORTO_OPTIONAL)) != 0)
            : false
            ;

    if (m && (add_modifiers || ut_ll_count(m->tags) || m->unit)) {
        bool first = true;
        ut_strbuf_appendstr(&data->memberBuff, ",{");
        if (add_modifiers) {
            ut_strbuf_appendstr(&data->memberBuff, "\"m\":\"");
            if ((m->modifiers & CORTO_READONLY) == CORTO_READONLY) {
                ut_strbuf_appendstr(&data->memberBuff, "r");
            }
            if ((m->modifiers & CORTO_CONST) == CORTO_CONST) {
                ut_strbuf_appendstr(&data->memberBuff, "c");
            }
            if ((m->modifiers & CORTO_KEY) == CORTO_KEY) {
                ut_strbuf_appendstr(&data->memberBuff, "k");
            }
            if ((m->modifiers & CORTO_OPTIONAL) == CORTO_OPTIONAL) {
                ut_strbuf_appendstr(&data->memberBuff, "o");
            }
            ut_strbuf_appendstr(&data->memberBuff, "\"");
            first = false;
        }
        if (m->unit) {
            if (!first) {
                ut_strbuf_appendstr(&data->memberBuff, ",");
            }
            ut_strbuf_append(
                &data->memberBuff,
                "\"u\":[\"%s\",\"%s\",\"%s\"]",
                corto_idof(m->unit->quantity),
                corto_idof(m->unit),
                m->unit->symbol);
        }
        if (ut_ll_count(m->tags)) {
            if (!first) {
                ut_strbuf_appendstr(&data->memberBuff, ",");
            }
            ut_strbuf_appendstr(&data->memberBuff, "\"t\":[");
            ut_iter it = ut_ll_iter(m->tags);
            int count = 0;
            while (ut_iter_hasNext(&it)) {
                corto_tag t = ut_iter_next(&it);
                if (count) {
                    ut_strbuf_appendstr(&data->memberBuff, ",");
                }
                ut_strbuf_append(&data->memberBuff, "\"%s\"",
                    corto_path(NULL, tags_o, t, "/"));
            }
            ut_strbuf_appendstr(&data->memberBuff, "]");
        }
        ut_strbuf_appendstr(&data->memberBuff, "}");
    }
    ut_strbuf_appendstr(&data->memberBuff, "]");

    corto_dealloc(escaped);

    data->count ++;

    ws_data_addMetadata(data->session, data->msg, type);

    return 0;
}

int16_t ws_typeSerializer_constant(
    corto_walk_opt* s,
    corto_value *info,
    void *userData)
{
    ws_typeSerializer_t *data = userData;
    corto_constant *c = info->is.constant.constant;

    if (!data->dataType->constants) {
        data->dataType->constants = corto_alloc(sizeof(ut_ll));
        *data->dataType->constants = ut_ll_new();
    }
    corto_stringList__append(*data->dataType->constants, corto_idof(c));
    return 0;
}

int16_t ws_typeSerializer_element(
    corto_walk_opt* s,
    corto_value *info,
    void *userData)
{
    ws_typeSerializer_t *data = userData;
    corto_collection type = corto_collection(corto_value_typeof(info));
    data->dataType->element_type = corto_calloc(sizeof(char*));
    {
            corto_id element_typeId;
            corto_set_str(data->dataType->element_type, corto_fullpath(element_typeId, type->element_type));
    }
    ws_data_addMetadata(data->session, data->msg, type->element_type);
    return 0;
}

static corto_walk_opt ws_typeSerializer(void) {
    corto_walk_opt result;

    corto_walk_init(&result);
    result.access = CORTO_PRIVATE;
    result.accessKind = CORTO_NOT;
    result.aliasAction = CORTO_WALK_ALIAS_IGNORE;
    result.optionalAction = CORTO_WALK_OPTIONAL_ALWAYS;
    result.metaprogram[CORTO_BASE] = ws_typeSerializer_member;
    result.metaprogram[CORTO_MEMBER] = ws_typeSerializer_member;
    result.metaprogram[CORTO_CONSTANT] = ws_typeSerializer_constant;
    result.program[CORTO_COLLECTION] = ws_typeSerializer_element;

    return result;
}

static
ws_dataType* ws_data_findDataType(
    ws_data *data,
    corto_type type)
{
    if (ut_ll_count(data->data)) {
        corto_id typeId;
        ws_type_identifier(type, typeId);
        ut_iter it = ut_ll_iter(data->data);
        while (ut_iter_hasNext(&it)) {
            ws_dataType *dataType = ut_iter_next(&it);
            if (!strcmp(dataType->type, typeId)) {
                return dataType;
            }
        }
    }
    return NULL;
}

static
ws_dataType* ws_data_addMetadata(
    ws_service_Session session,
    ws_data *msg,
    corto_type t)
{
    bool appendType = false;

    ws_dataType *dataType = ws_data_findDataType(msg, t);
    if (!dataType) {
        dataType = corto_calloc(sizeof(ws_dataType));
        corto_id typeId;
        ws_type_identifier(t, typeId);
        corto_set_str(&dataType->type, typeId);
        appendType = true;
    }

    if (!ut_ll_hasObject(session->typesAligned, t)) {
        corto_type kind = corto_typeof(t);
        corto_string__set(dataType->kind, corto_fullpath(NULL, kind));

        /* Don't treat range types as composites */
        if (kind != (corto_type)corto_range_o) {
            corto_walk_opt s = ws_typeSerializer();
            ws_typeSerializer_t walkData = {session, msg, dataType, UT_STRBUF_INIT, 0};
            ut_ll_insert(session->typesAligned, t);
            corto_metawalk(&s, t, &walkData);
            if (walkData.count) {
                ut_strbuf_appendstr(&walkData.memberBuff, "]");
                corto_string members = ut_strbuf_get(&walkData.memberBuff);
                corto_string__set(dataType->members, members);
                corto_dealloc(members);
            }
            if (t->reference) {
                corto_bool__set(dataType->reference, TRUE);
            }
        }
    }

    if (appendType) {
        ut_ll_append(msg->data, dataType);
    }

    return dataType;
}

void ws_service_Session_Subscription_processEvents(
    ws_service_Session_Subscription this)
{
    ws_service_Session session = corto_parentof(corto_parentof(this));

    ut_debug("ws: prepare %d events for '%s' [%p]",
        ut_ll_count(this->batch),
        corto_fullpath(NULL, this), this);

    ws_data *msg = corto_declare(NULL, NULL, ws_data_o);
    corto_set_str(&msg->sub, corto_idof(this));

    corto_subscriber_event *e;

    while ((e = ut_ll_takeFirst(this->batch))) {
        ws_dataObject *dataObject = NULL;
        void *data = (void*)e->data.value;

        corto_type t = corto_resolve(NULL, e->data.type);
        if (!t) {
            ut_error("unresolved type '%s' for object '%s/%s'", e->data.type, e->data.parent, e->data.id);
            goto error;
        }

        if (!corto_instanceof(corto_type_o, t)) {
            ut_error("object '%s' resolved by '%s' as type for '%s/%s' is not a type",
                corto_fullpath(NULL, t),
                e->data.type,
                e->data.parent,
                e->data.id);
            goto error;
        }

        ws_dataType *dataType = ws_data_addMetadata(session, msg, t);

        corto_eventMask mask = e->event;
        if (mask & (CORTO_UPDATE|CORTO_DEFINE)) {
            if (!dataType->set) {
                dataType->set = corto_alloc(sizeof(ut_ll));
                *dataType->set = ut_ll_new();
            }
            dataObject = ws_dataObjectList__append_alloc(*dataType->set);
        } else if (mask & CORTO_DELETE) {
            if (!dataType->del) {
                dataType->del = corto_alloc(sizeof(ut_ll));
                *dataType->del = ut_ll_new();
            }
            dataObject = ws_dataObjectList__append_alloc(*dataType->del);
        }

        if (dataObject) {
            corto_set_str(&dataObject->id, e->data.id);
            if (strcmp(e->data.parent, ".")) {
                corto_string__set(dataObject->p, e->data.parent);
            }

            /* Set owner if owner is remote */
            if (e->data.owner) {
                corto_id ownerId;
                if (corto_instanceof(corto_mount_o, e->data.owner)) {
                    if (corto_mount(e->data.owner)->ownership == CORTO_REMOTE_SOURCE) {
                        corto_string__set(dataObject->s, corto_fullpath(ownerId, e->data.owner));
                    }
                }
            }

            /* Mark object as readonly if not writable or builtin */
            if (e->data.object) {
                char attr[3] = {0};
                if (!corto_check_attr(e->data.object, CORTO_ATTR_WRITABLE) ||
                     corto_isbuiltin(e->data.object))
                {
                    strcat(attr, "r");
                }
                if (!corto_check_state(e->data.object, CORTO_VALID)) {
                    strcat(attr, "i");
                }
                if (strlen(attr)) {
                    corto_string__set(dataObject->a, attr);
                }
            }

            /* Don't serialize for DELETE */
            if (data && !(mask & CORTO_DELETE)) {
                corto_value v = corto_value_mem((void*)data, t);
                corto_string value = ws_serializer_serialize(&v, this->summary);
                if (value) {
                    corto_string__set(dataObject->v, NULL);
                    *(corto_string*)dataObject->v = value;
                }
            }
        }

        ut_assert(corto_release(e) == 0, "event is leaking");

        corto_release(t);
    }

    corto_define(msg);

    ws_service_Session_send(session, msg);
error:
    corto_delete(msg);
}
