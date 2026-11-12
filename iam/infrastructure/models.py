"""Peewee ORM model for the IAM bounded context.

Defines the ``devices`` database table used to persist
:class:`~iam.domain.entities.Device` aggregate roots.  This module belongs
to the infrastructure layer; it must not be imported directly by the domain
or application layers — access is mediated through the repository.
"""
from peewee import Model, CharField, DateTimeField

from shared.infrastructure.database import db


class Device(Model):
    """ORM mapping for the ``devices`` table.

    Each row represents a registered SafeCore device and its associated
    one-way API-key verifier, which is used to authenticate inbound requests.

    Attributes:
        device_id (CharField): Natural primary key — the human-readable
            device identifier (the UUID registered in SafeCore).
        credential_verifier (CharField): Salted, one-way verifier paired
            with the device credential.
        created_at (DateTimeField): UTC timestamp recording when the device
            was first registered.
        revoked_at (DateTimeField): UTC timestamp at which the device was
            revoked, or ``None`` while it remains active.
    """

    device_id  = CharField(primary_key=True)
    credential_verifier = CharField(column_name="api_key")
    created_at = DateTimeField()
    revoked_at = DateTimeField(null=True)

    class Meta:
        """Peewee metadata: binds the model to the shared database and names the table."""

        database   = db
        table_name = 'devices'
