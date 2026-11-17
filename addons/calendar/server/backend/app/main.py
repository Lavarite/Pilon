import os
import secrets
from zoneinfo import ZoneInfoNotFoundError

from fastapi import Depends, FastAPI, HTTPException, Request
from fastapi.security import HTTPAuthorizationCredentials, HTTPBearer
from fastapi.responses import JSONResponse

from .calendar import PublicationConflict, Store, occurrences

app = FastAPI(docs_url=None, redoc_url=None, openapi_url=None)
store = Store(os.environ.get("PILON_SHARE_DATABASE", "/data/calendar.sqlite"))
publisher_key = os.environ.get("PILON_PUBLISHER_KEY", "")
if len(publisher_key) < 32:
    raise RuntimeError("Configure PILON_PUBLISHER_KEY before starting the service")
bearer = HTTPBearer(auto_error=False)


@app.middleware("http")
async def privacy_headers(request: Request, call_next):
    try:
        length = int(request.headers.get("content-length", "0"))
    except ValueError:
        return JSONResponse({"detail": "Invalid request length"}, status_code=400)
    if length > 8 * 1024 * 1024:
        return JSONResponse({"detail": "Publication is too large"}, status_code=413)
    response = await call_next(request)
    response.headers.update({"Cache-Control": "no-store", "Referrer-Policy": "no-referrer",
                             "X-Content-Type-Options": "nosniff", "X-Robots-Tag": "noindex, nofollow"})
    return response


def publisher(credentials: HTTPAuthorizationCredentials = Depends(bearer)):
    if credentials is None or not secrets.compare_digest(credentials.credentials, publisher_key):
        raise HTTPException(401, "Publisher authentication required")


@app.get("/api/health")
def health():
    return {"ok": True}


@app.post("/api/shares", dependencies=[Depends(publisher)])
def create(payload: dict):
    try:
        return {"id": store.create(payload)}
    except (ValueError, KeyError, TypeError, ZoneInfoNotFoundError) as error:
        raise HTTPException(422, "Invalid calendar publication") from error


@app.put("/api/shares/{token}", dependencies=[Depends(publisher)])
def update(token: str, payload: dict, create: bool = False):
    try:
        if create:
            return {"id": store.create(payload, token)}
        if not store.update(token, payload):
            raise HTTPException(404, "Share link is unavailable")
        return {"ok": True}
    except PublicationConflict as error:
        raise HTTPException(409, "Read token belongs to another publication") from error
    except (ValueError, KeyError, TypeError, ZoneInfoNotFoundError) as error:
        raise HTTPException(422, "Invalid calendar publication") from error


@app.delete("/api/shares/{token}", dependencies=[Depends(publisher)])
def delete(token: str):
    store.delete(token)
    return {"ok": True}


@app.get("/api/calendars/{token}")
def calendar(token: str, start: str, end: str):
    payload = store.get(token)
    if payload is None:
        raise HTTPException(404, "Share link is unavailable")
    try:
        return {"preferences": payload["preferences"], "fields": payload["fields"], "publishedAt": payload.get("publishedAt"),
                "range": {"from": payload["from"], "through": payload["through"]},
                "events": occurrences(payload, start, end)}
    except (ValueError, KeyError, TypeError) as error:
        raise HTTPException(422, "Invalid calendar range") from error
