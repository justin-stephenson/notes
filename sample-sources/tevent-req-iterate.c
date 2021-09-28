#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <tevent.h>
#include <talloc.h>
#include <curl/curl.h>



/*
 * Modeled after
 * sdap_search_bases_ex_send()
 * sdap_search_bases_ex_next_base()
 * sdap_search_bases_ex_done()
 *
 * from src/providers/ldap/sdap_ops.c */

/* Taken from SSSD src/util/util.h */
#define TEVENT_REQ_RETURN_ON_ERROR(req) do { \
    enum tevent_req_state TRROEstate; \
    uint64_t TRROEuint64; \
    int TRROEerr; \
    \
    if (tevent_req_is_error(req, &TRROEstate, &TRROEuint64)) { \
        TRROEerr = (int)TRROEuint64; \
        switch (TRROEstate) { \
            case TEVENT_REQ_USER_ERROR:  \
                if (TRROEerr == 0) { \
                    return EIO; \
                } \
                return TRROEerr; \
            case TEVENT_REQ_TIMED_OUT: \
                return ETIMEDOUT; \
            default: \
                return EIO; \
        } \
    } \
} while (0)

/* Subrequest */
struct curl_execute_state {
    CURLcode res;
};

struct tevent_req *curl_execute_send(TALLOC_CTX *mem_ctx,
                                     struct tevent_context *ev,
                                     const char *url)
{
    struct curl_execute_state *state;
    struct tevent_req *req;
    CURL *curl;
    CURLcode res;

    req = tevent_req_create(mem_ctx, &state, struct curl_execute_state);
    if (req == NULL) {
        printf("tevent_req_create() failed\n");
        return NULL;
    }

    curl = curl_easy_init();
	if (curl) {
		curl_easy_setopt(curl, CURLOPT_URL, url);
		curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
		curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);

		/* Perform the request, res will get the return code */
		res = curl_easy_perform(curl);
		/* always cleanup */
		curl_easy_cleanup(curl);
	}

	/* We don't actually need this, as it is returned in tevent_req_error */
	state->res = res;

    if (res != CURLE_OK) {
        tevent_req_error(req, res);
        tevent_req_post(req, ev);
        return req;
    }

    /* In this minimal program, this request finishes without waiting for
     * an external event. Therefore ensure the callback will be executed,
     * even before caller sets it */
    tevent_req_post(req, ev);

    /* Mark request as done: req->state>=TEVENT_REQ_DONE
     * Nothing is freed here */
    tevent_req_done(req);

    return req;
}

/* _done() function is not needed as we are not doing async processing */
int curl_execute_recv(TALLOC_CTX *mem_ctx,
                        struct tevent_req *req,
                        CURLcode *_res)
{
    struct curl_execute_state *state = NULL;
    state = tevent_req_data(req, struct curl_execute_state);

    /* Talloc steal any dynamic memory here */
    *_res = state->res;

	TEVENT_REQ_RETURN_ON_ERROR(req);
    return 0;
}

/* Parent request */
struct transfer_file_state {
    struct tevent_context *ev;
    int num_iter;
    CURLcode res;
    const char *url;
};

static int transfer_file_next(struct tevent_req *req);
static void transfer_file_done(struct tevent_req *subreq);

struct tevent_req *transfer_file_send(TALLOC_CTX *mem_ctx,
                                      struct tevent_context *ev,
                                      const char *url)
{
    struct transfer_file_state *state;
    struct tevent_req *req;
    int ret;

    req = tevent_req_create(mem_ctx, &state, struct transfer_file_state);
    if (req == NULL) {
        printf("tevent_req_create() failed\n");
        ret = ENOMEM;
        goto done;
    }

    state->ev = ev;
    state->url = url;

    state->num_iter = 0;
    ret = transfer_file_next(req);
    if (ret == EAGAIN) {
        /* async processing */
        return req;
    }
done:
    if (ret == 0) {
        tevent_req_done(req);
    } else {
        tevent_req_error(req, ret);
    }
    tevent_req_post(req, ev);

    return req;
}    

static int transfer_file_next(struct tevent_req *req)
{
    struct transfer_file_state *state;
    struct tevent_req *subreq;

    state = tevent_req_data(req, struct transfer_file_state);

    /* Exit condition */
    if (state->num_iter == 3) {
        return 0;
    }

    printf("Sending Curl request [%d] for [%s]\n", state->num_iter, state->url);
    subreq = curl_execute_send(state, state->ev, state->url);
    if (subreq == NULL) {
        return ENOMEM;
    }

    tevent_req_set_callback(subreq, transfer_file_done, req);

    state->num_iter++;
    return EAGAIN;
}

static void transfer_file_done(struct tevent_req *subreq)
{
    struct transfer_file_state *state;
    struct tevent_req *req;
    int ret;

    req = tevent_req_callback_data(subreq, struct tevent_req);
    state = tevent_req_data(req, struct transfer_file_state);

    printf("Receiving Curl response\n");
    ret = curl_execute_recv(state, subreq, &state->res);
    talloc_free(subreq);
    if (ret != 0) {
        tevent_req_error(req, ret);
        return;
    }

    /* Try next file transfer */
    ret = transfer_file_next(req);
    if (ret == 0) {
        tevent_req_done(req);
    } else if (ret != EAGAIN) {
        tevent_req_error(req, ret);
    }

    return;
}

int transfer_file_recv(TALLOC_CTX *mem_ctx,
                        struct tevent_req *req,
                        CURLcode *_res)
{
    struct transfer_file_state *state = NULL;
    state = tevent_req_data(req, struct transfer_file_state);

    /* talloc steal any dynamic memory here */
    *_res = state->res;

    TEVENT_REQ_RETURN_ON_ERROR(req);

    return 0;
}

/* Caller code */
struct main_ctx {
    const char *url;
};
int caller(TALLOC_CTX *mem_ctx,
           struct tevent_context *ev,
           struct main_ctx *mctx);
void caller_done(struct tevent_req *req);

int caller(TALLOC_CTX *mem_ctx,
           struct tevent_context *ev,
           struct main_ctx *mctx)
{
    struct tevent_req *req;

    req = transfer_file_send(mem_ctx, ev, mctx->url);
    if (req == NULL) {
        return 1;
    }

    tevent_req_set_callback(req, caller_done, mctx);

    return 0;
}

void caller_done(struct tevent_req *req)
{
    int ret;
    CURLcode res;
    struct main_ctx *mctx;

    mctx = tevent_req_callback_data(req, struct main_ctx);

	/* CURLcode result is returned here up from curl_execute_recv()
	 * We don't actually need res */
    ret = transfer_file_recv(mctx, req, &res);
    talloc_free(req);
    if (ret != 0) {
        printf("Transfer_file failure\n");
		if (ret == EIO) {
			printf("Tevent request EIO\n");
		} else if (ret == ETIMEDOUT) {
			printf("Tevent request ETIMEDOUT\n");
		}
        if (ret != CURLE_OK) {
            fprintf(stderr, "curl_easy_perform() failed: %s\n",
                    curl_easy_strerror(ret));
        }
        return;
    } else {
        printf("Transfer successful!\n");
    }
}

int main(void) {

    int ret;
    struct tevent_context *ev;
    //const char *url = "http://10.255.255.1";     // invalid URL for testing
    const char *url = "https://example.com";
    struct main_ctx *mctx;

    /* The event context is the top level structure.
     * Everything else should hang off that */
    ev = tevent_context_init(NULL);
    if (ev == NULL) {
        printf("Event context init failure \n");
        return 1;
    }

    mctx = talloc_zero(ev, struct main_ctx);
    if (ev == NULL) {
        printf("ENOMEM \n");
        return 1;
    }

    mctx->url = url;

    ret = caller(mctx, ev, mctx);
    if (ret != 0) {
        printf("Caller returned unsuccessful\n");
        return ret;
    }

    tevent_loop_wait(ev);

    talloc_free(mctx);
    return 0;

}
