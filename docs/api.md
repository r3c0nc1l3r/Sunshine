# API

Sunshine has a RESTful API which can be used to interact with the service.

Unless otherwise specified, authentication is required for all API calls. You can authenticate using
basic authentication with the admin username and password.

## CSRF Protection

State-changing API endpoints (POST, DELETE) are protected against Cross-Site Request Forgery (CSRF) attacks.

**For Web Browsers:**
- Requests from same-origin (configured via `csrf_allowed_origins`) are automatically allowed
- Cross-origin requests require a CSRF token

**For Non-Browser Applications:**
- Non-browser clients (e.g. `curl`, scripts, custom apps) are **exempt** from CSRF protection
- CSRF attacks require a browser to silently attach credentials to a cross-origin request — this threat
  does not apply to non-browser clients that explicitly provide credentials with every request
- Requests with no `Origin` or `Referer` header (as is typical for non-browser clients) are automatically
  allowed without a CSRF token

**Example (browser-equivalent cross-origin request):**
```bash
# Get CSRF token
curl -u user:pass https://localhost:47990/api/csrf-token

# Use token in request
curl -u user:pass -H "X-CSRF-Token: your_token_here" \
  -X POST https://localhost:47990/api/restart
```

@htmlonly
<script src="api.js"></script>
@endhtmlonly

## GET /api/csrf-token
@copydoc confighttp::getCSRFToken()

## GET /api/apps
@copydoc confighttp::getApps()

## POST /api/apps
@copydoc confighttp::saveApp()

## POST /api/apps/close
@copydoc confighttp::closeApp()

## DELETE /api/apps/{index}
@copydoc confighttp::deleteApp()

## GET /api/browse
@copydoc confighttp::browseDirectory()

## GET /api/clients/list
@copydoc confighttp::getClients()

## POST /api/clients/unpair
@copydoc confighttp::unpair()

## POST /api/clients/unpair-all
@copydoc confighttp::unpairAll()

## POST /api/clients/update
@copydoc confighttp::updateClient()

## GET /api/config
@copydoc confighttp::getConfig()

## GET /api/configLocale
@copydoc confighttp::getLocale()

## POST /api/config
@copydoc confighttp::saveConfig()

## GET /api/covers/{index}
@copydoc confighttp::getCover()

## POST /api/covers/upload
@copydoc confighttp::uploadCover()

## GET /api/logs
@copydoc confighttp::getLogs()

## POST /api/password
@copydoc confighttp::savePassword()

## POST /api/pin
@copydoc confighttp::savePin()

## POST /api/reset-display-device-persistence
@copydoc confighttp::resetDisplayDevicePersistence()

## POST /api/reset-portal-token
@copydoc confighttp::resetPortalToken()

## POST /api/restart
@copydoc confighttp::restart()

## GET /api/sessions
Lists the sessions that are running or stopping, with `id`, `started` (Unix seconds), `transport` (`udp/ipv4` or
`udp/ipv6`) and `client_category` (`local`, `lan` or `wan`). No client names, addresses, certificates or keys are returned.

This endpoint exists only on the admin web UI listener (the same port as the configuration pages). It is not served on
the game stream ports.

Responses:
- `200`: `{"status": true, "sessions": [...]}`. The list is empty when no session is running.
- `401`: no credentials, or wrong username or password.
- `403`: the client address is not allowed.

If no admin user is configured yet, both session endpoints redirect to the welcome page and do nothing.

Session ids are sequential and not secret. Access is protected by admin authentication, not by the id.
@copydoc confighttp::getSessions()

## POST /api/sessions/{id}/close
Ends one session through the normal stop path, with the stop reason `api-close`. `{id}` is the decimal session id from
`GET /api/sessions`.

This endpoint exists only on the admin web UI listener and requires admin authentication. Cross-origin requests also need
a valid CSRF token (see [CSRF Protection](#csrf-protection)).

Responses:
- `200`: `{"status": true}`. The session exists and was asked to stop.
- `400`: `{id}` is not a 32-bit unsigned decimal number, or the CSRF token is missing, invalid or expired.
- `401`: no credentials, or wrong username or password.
- `403`: the client address is not allowed.
- `404`: no session with this id is running or stopping (unknown, not yet started, or already ended).

If no admin user is configured yet, the request redirects to the welcome page and nothing is done.

Session ids are sequential and not secret. Access is protected by admin authentication, not by the id.

Every request that passes authentication is logged at Info, including requests rejected by the CSRF check:

```
Session close requested via API: id=<id>, user=<admin user>, source=<client address>, result=<result>
```

- `id`: the requested id. For `bad-id` and `csrf-rejected`, it is the sanitised path id (digits only, at most 32 characters).
- `user`: the authenticated admin user.
- `source`: the TCP peer address of the client. A forwarded-for header is never used.
- `result`: `closed` (the session was asked to stop), `not-found` (no running or stopping session has this id),
  `csrf-rejected` (the CSRF check failed), or `bad-id` (the id is not a valid 32-bit unsigned number).

The password, the `Authorization` header and the CSRF token never appear in the log. Unauthenticated requests are
rejected before this line is written.
@copydoc confighttp::closeSession()

## GET /api/virtual-input/status
@copydoc confighttp::getVirtualInputStatus()

<div class="section_buttons">

| Previous                                    |                                  Next |
|:--------------------------------------------|--------------------------------------:|
| [Performance Tuning](performance_tuning.md) | [Troubleshooting](troubleshooting.md) |

</div>

<details style="display: none;">
  <summary></summary>
  [TOC]
</details>
