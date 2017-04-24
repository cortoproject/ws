/* $CORTO_GENERATED
 *
 * Server_Session_Subscription.c
 *
 * Only code written between the begin and end tags will be preserved
 * when the file is regenerated.
 */

#include <corto/ws/ws.h>

corto_void _ws_Server_Session_Subscription_addEvent(
    ws_Server_Session_Subscription this,
    corto_event e)
{
/* $begin(corto/ws/Server/Session/Subscription/addEvent) */

    corto_llAppend(this->batch, e);

/* $end */
}

/* $header(corto/ws/Server/Session/Subscription/processEvents) */
typedef struct ws_typeSerializer_t {
    ws_Server_Session session;
    ws_data *msg;
    ws_dataType *dataType;
    corto_buffer memberBuff;
    corto_int32 count;
} ws_typeSerializer_t;

ws_dataType* ws_data_addMetadata(
    ws_Server_Session session, 
    ws_data *msg, 
    corto_type t);

corto_int16 ws_typeSerializer_member(corto_serializer s, corto_value *info, void *userData) {
    ws_typeSerializer_t *data = userData;
    corto_type type = corto_value_getType(info);
    corto_member m = info->is.member.t;

    if (data->count) {
        corto_buffer_appendstr(&data->memberBuff, ",");
    } else {
         corto_buffer_appendstr(&data->memberBuff, "{");   
    }

    corto_id typeId;
    corto_fullpath(typeId, type);
    corto_string escaped = ws_serializer_escape(typeId);
    corto_buffer_append(&data->memberBuff, "\"%s\":\"%s\"", corto_idof(m), escaped);
    corto_dealloc(escaped);

    data->count ++;

    ws_data_addMetadata(data->session, data->msg, type);

    return 0;
}

corto_int16 ws_typeSerializer_constant(corto_serializer s, corto_value *info, void *userData) {
    ws_typeSerializer_t *data = userData;
    corto_type type = corto_value_getType(info);
    corto_constant *c = info->is.constant.t;

    if (!data->dataType->constants) {
        data->dataType->constants = corto_alloc(sizeof(corto_ll));
        *data->dataType->constants = corto_llNew();
    }
    corto_stringListAppend(*data->dataType->constants, corto_idof(c));
    ws_data_addMetadata(data->session, data->msg, type);
    return 0;
}

corto_int16 ws_typeSerializer_element(corto_serializer s, corto_value *info, void *userData) {
    ws_typeSerializer_t *data = userData;
    corto_type type = corto_value_getType(info);
    data->dataType->elementType = corto_alloc(sizeof(corto_object));
    corto_setref(data->dataType->elementType, type);
    ws_data_addMetadata(data->session, data->msg, type);
    return 0;
}

static struct corto_serializer_s ws_typeSerializer(void) {
    struct corto_serializer_s result;

    corto_serializerInit(&result);
    result.access = CORTO_PRIVATE;
    result.accessKind = CORTO_NOT;
    result.aliasAction = CORTO_SERIALIZER_ALIAS_IGNORE;
    result.optionalAction = CORTO_SERIALIZER_OPTIONAL_ALWAYS;
    result.metaprogram[CORTO_MEMBER] = ws_typeSerializer_member;
    result.metaprogram[CORTO_CONSTANT] = ws_typeSerializer_constant;
    result.metaprogram[CORTO_ELEMENT] = ws_typeSerializer_element;

    return result;
}

static ws_dataType* ws_data_findDataType(ws_data *data, corto_type type) {
    corto_iter it = corto_llIter(data->data);
    while (corto_iterHasNext(&it)) {
        ws_dataType *dataType = corto_iterNext(&it);
        if (dataType->type == type) {
            return dataType;
        }
    }
    return NULL;
}

ws_dataType* ws_data_addMetadata(
    ws_Server_Session session, 
    ws_data *msg, 
    corto_type t) 
{ 
    ws_dataType *dataType = ws_data_findDataType(msg, t);
    if (!dataType) {
        dataType = ws_dataTypeListInsertAlloc(msg->data);
        corto_setref(&dataType->type, t);
    }

    if (!corto_llHasObject(session->typesAligned, t)) {
        corto_type kind = corto_typeof(t);
        corto_stringSet(dataType->kind, corto_fullpath(NULL, kind));
        struct corto_serializer_s s = ws_typeSerializer();
        ws_typeSerializer_t walkData = {session, msg, dataType, CORTO_BUFFER_INIT, 0};
        corto_llInsert(session->typesAligned, t);
        corto_metaWalk(&s, t, &walkData);
        if (walkData.count) {
            corto_buffer_appendstr(&walkData.memberBuff, "}");
            corto_string members = corto_buffer_str(&walkData.memberBuff);
            corto_stringSet(dataType->members, members);
            corto_dealloc(members);
        }
        if (t->reference) {
            corto_boolSet(dataType->reference, TRUE);
        }
    }

    return dataType;
}

/* $end */
corto_void _ws_Server_Session_Subscription_processEvents(
    ws_Server_Session_Subscription this)
{
/* $begin(corto/ws/Server/Session/Subscription/processEvents) */
    ws_Server_Session session = corto_parentof(corto_parentof(this));

    corto_trace("ws: prepare %d events for '%s' [%p]",
        corto_llSize(this->batch),
        corto_fullpath(NULL, this), this);

    ws_data *msg = corto_declare(ws_data_o);
    corto_setstr(&msg->sub, corto_idof(this));

    corto_subscriberEvent e;
    while ((e = corto_llTakeFirst(this->batch))) {
        ws_dataObject *dataObject = NULL;
        corto_object o = e->result.object;
        if (!o) {
            corto_warning("ws: event for '%s' does not have an object reference", e->result.id);
            corto_release(e);
            continue;
        }

        corto_type t = corto_typeof(o);
        ws_dataType *dataType = ws_data_addMetadata(session, msg, t);

        corto_eventMask mask = corto_observableEvent(e)->mask;
        if (mask & (CORTO_ON_UPDATE|CORTO_ON_DEFINE)) {
            if (!dataType->set) {
                dataType->set = corto_alloc(sizeof(corto_ll));
                *dataType->set = corto_llNew();
            }
            dataObject = ws_dataObjectListAppendAlloc(*dataType->set);
        } else if (mask & CORTO_ON_DELETE) {
            if (!dataType->del) {
                dataType->del = corto_alloc(sizeof(corto_ll));
                *dataType->del = corto_llNew();
            }
            dataObject = ws_dataObjectListAppendAlloc(*dataType->del);   
        }

        if (dataObject) {
            corto_setstr(&dataObject->id, e->result.id);
            if (strcmp(e->result.parent, ".")) {
                corto_stringSet(dataObject->p, e->result.parent);
            }

            corto_string value = ws_serializer_serialize(e->result.object);
            if (value) {
                corto_stringSet(dataObject->v, NULL);
                *(corto_string*)dataObject->v = value;
            }
        }

        corto_assert(corto_release(e) == 0, "event is leaking");
    }

    corto_define(msg);

    ws_Server_Session_send(session, msg);

    corto_delete(msg);

/* $end */
}
