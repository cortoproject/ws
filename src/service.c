/* This is a managed file. Do not delete this comment. */

#include <corto/ws/ws.h>
static 
void ws_service_onConnect(
    ws_service this, 
    httpserver_HTTP_Connection c, 
    ws_connect *clientMsg) 
{
    corto_tableinstance sessions = corto_lookupAssert(this, "Session", corto_tableinstance_o);
    ws_service_Session session = NULL;
    corto_object msg = NULL;

    if (clientMsg->version && strcmp(clientMsg->version, "1.0")) {
        msg = ws_failedCreate("1.0");
        corto_warning("connect: wrong version '%s'", clientMsg->version);
    } else {
        if (!clientMsg->session || !(session = corto_lookup(sessions, clientMsg->session))) {
            char *sessionId = httpserver_random(17);
            session = ws_service_SessionCreateChild(sessions, sessionId);
            corto_ptr_setref(&session->conn, c);
            corto_ptr_setref(&c->ctx, session);
            corto_trace("connect: established session '%s'", sessionId);
            corto_dealloc(sessionId);
        } else {
            corto_ptr_setref(&session->conn, c);
            corto_ptr_setref(&c->ctx, session);
            corto_trace("connect: reestablished session '%s'", clientMsg->session);
            corto_release(session);
        }

        msg = ws_connectedCreate(corto_idof(session));
    }

    ws_service_Session_send(session, msg);
    corto_delete(msg);
}

static 
void ws_service_onSub(
    ws_service this, 
    httpserver_HTTP_Connection c, 
    ws_sub *clientMsg) 
{
    ws_service_Session session = ws_service_Session(c->ctx);
    corto_tableinstance subscriptions = corto_lookupAssert(session, "Subscription", corto_tableinstance_o);
    corto_object msg = NULL;

    /* If there is an existing subscription for the specified id, delete it. */
    service_Session_Subscription sub = corto_lookup(subscriptions, clientMsg->id);
    if (sub) {
        corto_delete(sub);
        corto_release(sub);
    }

    /* Create new subscription */
    sub = corto_declareChild(subscriptions, clientMsg->id, ws_service_Session_Subscription_o);
    if (!sub) {
        msg = ws_subfailCreate(corto_idof(sub), corto_lasterr());
        corto_error("creation of subscriber failed: %s", corto_lasterr());
    } else {
    
        /* Query parameters */
        corto_ptr_setstr(&sub->super.query.from, clientMsg->parent);
        corto_ptr_setstr(&sub->super.query.select, clientMsg->expr);
        corto_ptr_setstr(&sub->super.query.type, clientMsg->type);
        
        /*sub->offset = clientMsg->offset;
        sub->limit = clientMsg->limit;*/
        /* Set dispatcher & instance to session and server */
        corto_ptr_setref(&corto_observer(sub)->instance, session);
        corto_ptr_setref(&corto_observer(sub)->dispatcher, this);
        /* Enable subscriber so subok is sent before alignment data */
        corto_observer(sub)->enabled = FALSE;
        /* Set if subscription requests summary data */
        sub->summary = clientMsg->summary;
        if (corto_define(sub)) {
            msg = ws_subfailCreate(corto_idof(sub), corto_lasterr());
            corto_error("failed to create subscriber: %s", corto_lasterr());
            corto_delete(sub);
            sub = NULL;
        } else {
            msg = ws_subokCreate(corto_idof(sub));
            corto_trace("sub: subscriber '%s' listening to '%s', '%s'", 
                clientMsg->id, clientMsg->parent, clientMsg->expr);
        }

    }

    ws_service_Session_send(session, msg);
    corto_delete(msg);
    if (sub) {
        /* Enable subscriber, aligns data */
        if (corto_subscriber_subscribe(sub, session)) {
            corto_error("ws: failed to enable subscriber: %s", corto_lasterr());
        }

        /* Flush aligned data for quick response time */
        ws_service_flush(this, &sub->super);
    }

}

static 
void ws_service_onUnsub(
    ws_service this, 
    httpserver_HTTP_Connection c, 
    ws_unsub *clientMsg) 
{    
    ws_service_Session session = ws_service_Session(c->ctx);
    corto_tableinstance subscriptions = corto_lookupAssert(session, "Subscription", corto_tableinstance_o);

    /* If there is an existing subscription for the specified id, delete it. */
    corto_subscriber sub = corto_lookup(subscriptions, clientMsg->id);
    if (sub) {
        ws_service_purge(this, sub);
        corto_delete(sub);
        corto_release(sub);
    }

    corto_trace("unsub: deleted subscriber '%s'", clientMsg->id);
}

static 
void ws_service_onUpdate(
    ws_service this, 
    httpserver_HTTP_Connection c, 
    ws_update *updateMsg) 
{
    if (corto_publish(
        CORTO_UPDATE,
        updateMsg->id,
        NULL,
        "text/json",
        updateMsg->v ? *updateMsg->v : NULL
    )) {
        corto_error("update: failed to update '%s'", updateMsg->id);
    }

    return;
}

static 
void ws_service_onDelete(
    ws_service this, 
    httpserver_HTTP_Connection c, 
    ws_delete *deleteMsg) 
{
    if (corto_publish(
        CORTO_DELETE,
        deleteMsg->id,
        NULL,
        NULL,
        0
    )) {
        corto_error("delete: failed to delete '%s'", deleteMsg->id);
    }

    return;
}

void ws_service_flush(
    ws_service this,
    corto_subscriber sub)
{
    corto_subscriberEvent *e;
    corto_ll subs = corto_ll_new();

    /* Collect events */
    corto_lock(this);
    corto_iter it = corto_ll_iter(this->events);
    while (corto_iter_hasNext(&it)) {
        e = corto_iter_next(&it);

        /* Only take events for specified subscriber */
        if (!sub || (sub == e->subscriber)) {
            corto_ll_iterRemove(&it);

            /* It is possible that the session has already been deleted */
            if (!corto_checkState(e->instance, CORTO_DELETED)) {
                safe_ws_service_Session_Subscription_addEvent(e->subscriber, (corto_event*)e);
                if (!corto_ll_hasObject(subs, e->subscriber)) {
                    corto_ll_insert(subs, e->subscriber);
                }

            } else {
                corto_assert(corto_release(e) == 0, "event is leaking");
            }

        }

    }

    corto_unlock(this);
    /* Process events outside of lock */
    it = corto_ll_iter(subs);
    while (corto_iter_hasNext(&it)) {
        ws_service_Session_Subscription sub = corto_iter_next(&it);
        ws_service_Session_Subscription_processEvents(sub);
    }

    corto_ll_free(subs);
}

void ws_service_onClose(
    ws_service this,
    httpserver_HTTP_Connection c)
{
    if (c->ctx) {
        corto_trace("close: disconnected session '%s'", corto_idof(c->ctx));
        corto_delete(c->ctx);
        corto_ptr_setref(&c->ctx, NULL);
    }    
}

void ws_service_onMessage(
    ws_service this,
    httpserver_HTTP_Connection c,
    corto_string msg)
{
    corto_component_push("ws");
    corto_object o = corto_createFromContent("text/json", msg);
    if (!o || !corto_checkState(o, CORTO_VALID)) {
        corto_error("%s (malformed message)", corto_lasterr());
        goto error;
    }

    if (corto_typeof(corto_typeof(o)) != corto_type(corto_struct_o)) goto error_type;
    corto_struct msgType = corto_struct(corto_typeof(o));
    if (msgType == ws_connect_o) ws_service_onConnect(this, c, ws_connect(o));
    else if (msgType == ws_sub_o) ws_service_onSub(this, c, ws_sub(o));
    else if (msgType == ws_unsub_o) ws_service_onUnsub(this, c, ws_unsub(o));
    else if (msgType == ws_update_o) ws_service_onUpdate(this, c, ws_update(o));
    else if (msgType == ws_delete_o) ws_service_onDelete(this, c, ws_delete(o));
    else goto error_type;
    corto_delete(o);
    corto_component_pop();
    return;
error_type: 
    corto_error("received invalid message type: '%s'", corto_fullpath(NULL, corto_typeof(o)));
error:
    corto_component_pop();
    return;
}

void ws_service_onPoll(
    ws_service this)
{

    ws_service_flush(this, NULL);

}

#define WS_QUEUE_THRESHOLD 10000
#define WS_QUEUE_THRESHOLD_SLEEP 10000000
static corto_subscriberEvent* ws_service_findEvent(ws_service this, corto_subscriberEvent *e) {
    corto_iter iter = corto_ll_iter(this->events);
    corto_subscriberEvent *e2;
    while ((corto_iter_hasNext(&iter))) {
        e2 = corto_iter_next(&iter);
        if (!strcmp(e2->data.id, e->data.id) &&
            !strcmp(e2->data.parent, e->data.parent) &&
            (e2->subscriber == e->subscriber))
        {
            return e2;
        }

    }

    return NULL;
}

void ws_service_post(
    ws_service this,
    corto_event *e)
{
    corto_uint32 size = 0;
    corto_subscriberEvent *e2;

    corto_lock(this);

    /* Ignore events from destructed sessions or subscribers */
    corto_object 
        observer = ((corto_subscriberEvent*)e)->subscriber,
        instance = ((corto_subscriberEvent*)e)->instance;

    if (corto_checkState(observer, CORTO_DELETED) || 
        (instance && corto_checkState(instance, CORTO_DELETED))) 
    {
        corto_release(e);
        corto_unlock(this);
        return;
    }

    /* Check if there is already another event in the queue for the same object.
     * if so, replace event with latest update. */
    if ((e2 = ws_service_findEvent(this, corto_subscriberEvent(e)))) {
        corto_ll_replace(this->events, e2, e);
        corto_assert(corto_release(e2) == 0, "event is leaking");
    } else {
        corto_ll_append(this->events, e);
    }

    size = corto_ll_size(this->events);
    corto_unlock(this);
    /* If queue is getting big, slow down publisher */
    if (size > WS_QUEUE_THRESHOLD) {
        corto_sleep(0, WS_QUEUE_THRESHOLD_SLEEP);
    }

}

void ws_service_purge(
    ws_service this,
    corto_subscriber sub)
{

    /* Purge events for specified subscriber */
    corto_lock(this);
    corto_iter it = corto_ll_iter(this->events);
    while (corto_iter_hasNext(&it)) {
        corto_subscriberEvent *e = corto_iter_next(&it);
        if (e->subscriber == sub) {
            corto_release(e);
            corto_ll_iterRemove(&it);
        }

    }

    corto_unlock(this);
}
