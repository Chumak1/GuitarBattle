"""Security helpers around the Guitar Duels WebSocket.

The WebSocket handshake carries ``?token=<access_token>``. Uvicorn access logs
include the query string, so ``app.main`` installs a filter that masks it.
This test pins the redaction without touching the database.
"""
from __future__ import annotations

import logging

from app.main import _RedactAccessTokens


def test_access_log_filter_redacts_websocket_token():
    record = logging.LogRecord(
        name="uvicorn.access",
        level=logging.INFO,
        pathname="",
        lineno=0,
        msg='%s - "WebSocket %s" [accepted]',
        args=(
            "127.0.0.1:12345",
            "/api/v1/duels/9b8c7d6e-5f4a-3210-9fed-cba987654321/ws"
            "?token=super-secret-jwt&x=1",
        ),
        exc_info=None,
    )
    assert _RedactAccessTokens().filter(record) is True
    message = record.getMessage()
    assert "super-secret-jwt" not in message
    assert "token=***" in message


def test_access_log_filter_keeps_unrelated_paths():
    record = logging.LogRecord(
        name="uvicorn.access",
        level=logging.INFO,
        pathname="",
        lineno=0,
        msg='%s - "%s %s" %s',
        args=("127.0.0.1:1", "GET", "/api/v1/duels/history", "200"),
        exc_info=None,
    )
    _RedactAccessTokens().filter(record)
    message = record.getMessage()
    assert "/api/v1/duels/history" in message
    assert message.endswith("200")
