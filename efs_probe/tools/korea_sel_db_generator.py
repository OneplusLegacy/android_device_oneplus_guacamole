"""Offline-only exact-baseline Korea XML proposal generator. No CLI or I/O.

A confirmed mapping is caller-supplied evidence, not an index hypothesis.
This module has no modem/DIAG/Binder dependency and grants no write approval.
"""
from dataclasses import dataclass
from enum import Enum
import hashlib
import xml.etree.ElementTree as ET

KNOWN_SHA256 = "b81f1d34e8279547c789b6665e82db41a07d53451ea7a15bed7e628eb829f155"
KNOWN_SIZE = 57655


class EvidenceState(Enum):
    UNCONFIRMED = "UNCONFIRMED"
    CONFIRMED_FOR_SEL_DB_GENERATION = "CONFIRMED_FOR_SEL_DB_GENERATION"
    CONFIRMED = "CONFIRMED_FOR_SEL_DB_GENERATION"  # source-compatible alias


@dataclass(frozen=True)
class CarrierIndexEvidence:
    index: int | None = None
    state: EvidenceState = EvidenceState.UNCONFIRMED
    evidence_reference: str = ""


@dataclass(frozen=True)
class ConfirmedCarrierMapping:
    skt: CarrierIndexEvidence = CarrierIndexEvidence()
    kt: CarrierIndexEvidence = CarrierIndexEvidence()
    lgu: CarrierIndexEvidence = CarrierIndexEvidence()


@dataclass(frozen=True)
class Proposal:
    outcome: str
    original_sha256: str
    proposed_sha256: str | None = None
    proposed_bytes: bytes | None = None
    insertion_offset: int | None = None
    inserted_bytes: bytes = b""
    records_before: int | None = None
    records_after: int | None = None


class GenerationBlocked(ValueError):
    pass


# Identity constants only; indices exclusively from supplied evidence.
_CARRIERS = (("SKT", "898205", "450-05"),
             ("KT", "898230", "450-08"),
             ("LGU", "898206", "450-06"))
_COMMON = (b'  <if>\n    <tristate_reset_all return="true" />\n    <then>\n'
           b'       <SelRecord carrier_name="Common" iin="iin" imsi_3gpp="3gpp_imsi" '
           b'mcfg_carrier_index="8" />\n    </then>\n  </if>\n</policy>')


def _indices(mapping):
    if not isinstance(mapping, ConfirmedCarrierMapping):
        raise GenerationBlocked("GENERATE_BLOCKED_UNCONFIRMED_MAPPING")
    entries = (mapping.skt, mapping.kt, mapping.lgu)
    if any(not isinstance(e, CarrierIndexEvidence) or
           e.state is not EvidenceState.CONFIRMED or not e.evidence_reference.strip() or
           type(e.index) is not int or not 0 <= e.index <= 65535 for e in entries):
        raise GenerationBlocked("GENERATE_BLOCKED_UNCONFIRMED_MAPPING")
    values = tuple(e.index for e in entries)
    return values


def _parse(data):
    # No entities, DTDs, processing instructions beyond the fixed XML declaration.
    if b"<!DOCTYPE" in data or b"<!ENTITY" in data:
        raise GenerationBlocked("HARD_STOP_XML_STRUCTURE")
    try:
        root = ET.fromstring(data)
    except (ET.ParseError, ValueError) as exc:
        raise GenerationBlocked("HARD_STOP_XML_STRUCTURE") from exc
    if root.tag != "policy" or root.attrib != {"mcfg_db_ver": "1.2.3", "name": "generic"}:
        raise GenerationBlocked("HARD_STOP_XML_STRUCTURE")
    return root


def _block(indices):
    chunks = []
    for (carrier, iin, plmn), index in zip(_CARRIERS, indices):
        # Explicit Korea capability flags follow the reviewed predicate example.
        # Cross-device indices are NOT used; guacamole mapping remains separate.
        # Flags in a proposal do not establish runtime IMS/network capability.
        chunks.append(
            '  <if>\n    <any_of evaluate_all="true">\n'
            '      <tristate_reset_all return="false" />\n'
            f'      <iin_in not_present="false" store_in="iin">{iin}</iin_in>\n'
            f'      <imsi_3gpp_plmn_in not_present="false" store_in="3gpp_imsi">{plmn}</imsi_3gpp_plmn_in>\n'
            '    </any_of>\n    <then>\n'
            f'      <SelRecord carrier_name="{carrier}" country_code="410" iin="iin" '
            f'imsi_3gpp="3gpp_imsi" mcfg_carrier_index="{index}" volte="true" vowifi="false" />\n'
            '    </then>\n  </if>\n')
    return ''.join(chunks).encode('ascii')


def _validate_original(original, indices):
    if len(original) != KNOWN_SIZE or hashlib.sha256(original).hexdigest() != KNOWN_SHA256:
        raise GenerationBlocked("GENERATE_BLOCKED_BASELINE_SHA")
    root = _parse(original)
    common = [e for e in root.iter("SelRecord") if e.get("carrier_name") == "Common"]
    if len(common) != 1 or not original.endswith(_COMMON) or original.count(_COMMON) != 1:
        raise GenerationBlocked("HARD_STOP_COMMON_FALLBACK")
    records = list(root.iter("SelRecord"))
    # Carrier/category indices are reused by stock rules; they are NOT PKs.
    if any(e.get("carrier_name") in {c[0] for c in _CARRIERS} for e in records):
        raise GenerationBlocked("HARD_STOP_EXISTING_KOREA_IDENTITY")
    for _, iin, plmn in _CARRIERS:
        for element in root.iter():
            if element.tag in ("iin_in", "imsi_3gpp_plmn_in"):
                if iin in (element.text or '').split() or plmn in (element.text or '').split():
                    raise GenerationBlocked("HARD_STOP_EXISTING_KOREA_IDENTITY")
    return len(records)


def generate_korea_proposal(original: bytes, current: bytes,
                            mapping: ConfirmedCarrierMapping) -> Proposal:
    """Return canonical proposal, or fail closed without output bytes.

    `original` is the saved exact known baseline. `current` must be either
    that baseline or the entire exact canonical output for the SAME evidence.
    Full-byte equality (stronger than marker detection) enforces every record,
    index, field, order, uniqueness, and all untouched original XML bytes.
    """
    indices = _indices(mapping)
    count = _validate_original(original, indices)
    offset = len(original) - len(_COMMON)
    insertion = _block(indices)
    proposed = original[:offset] + insertion + original[offset:]
    parsed = _parse(proposed)
    if len(list(parsed.iter("SelRecord"))) != count + 3:
        raise GenerationBlocked("HARD_STOP_RECORD_COUNT")
    if current == original:
        outcome = "GENERATED"
    elif current == proposed:
        outcome = "ALREADY_PATCHED_EXACT"
    else:
        raise GenerationBlocked("HARD_STOP_PARTIAL_OR_UNKNOWN_STATE")
    return Proposal(outcome, KNOWN_SHA256, hashlib.sha256(proposed).hexdigest(),
                    proposed, offset, insertion, count, count + 3)


def guacamole_confirmed_mapping() -> ConfirmedCarrierMapping:
    """Device-scoped, generation-only evidence; never grants an EFS write permit."""
    state = EvidenceState.CONFIRMED_FOR_SEL_DB_GENERATION
    ref = "MACRO_B2_EVIDENCE.md: runtime versions + stock repeated indices + PDC1.9 enums"
    return ConfirmedCarrierMapping(CarrierIndexEvidence(39, state, ref),
                                   CarrierIndexEvidence(40, state, ref),
                                   CarrierIndexEvidence(38, state, ref))
