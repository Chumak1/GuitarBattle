"""Health and service metadata endpoints."""
import pytest


def test_liveness(client):
    response = client.get("/health/live")
    assert response.status_code == 200
    assert response.json() == {"status": "ok"}


@pytest.mark.db
def test_readiness_reports_database(db_client):
    response = db_client.get("/health/ready")
    assert response.status_code == 200
    assert response.json() == {"status": "ready", "database": "ok"}


def test_root_serves_landing_page(client):
    response = client.get("/")
    assert response.status_code == 200
    assert response.headers["content-type"].startswith("text/html")
    assert response.headers["cache-control"] == "no-cache, must-revalidate"
    body = response.text.lower()
    assert "fretfight" in body
    # / is the Russian document, rendered server-side (see test_landing_seo.py).
    assert 'lang="ru"' in body
    assert "гитарный батл" in body
    assert "online" in body
