# kg_interface.py
from rdflib import Graph, URIRef
from rdflib.namespace import RDF
import json

class KGInterface:
    def __init__(self, ontology_path: str,
                 ont_iri: str, class_prefix: str, op_prefix: str, dp_prefix: str):
        self.ontology_path = ontology_path
        self.ont_iri = ont_iri
        self.class_prefix = class_prefix
        self.op_prefix = op_prefix
        self.dp_prefix = dp_prefix
        self.graph = Graph()
        # TTL laden
        self.graph.parse(ontology_path, format="turtle")

    def getFailureModeParameters(self, interruptedSkill: str) -> str:
        """Gibt NUR rows-JSON zurück: [{id,t,v}, ...]
           v ist der Literalwert als String; t ist ein grober Typ (string|int|double|bool)."""
        searchSkillIri = self.ont_iri + ('' if self.ont_iri.endswith(('#','/')) else '#') + interruptedSkill
        query = f"""
            PREFIX cl: <{self.class_prefix}>
            PREFIX op: <{self.op_prefix}>
            PREFIX dp: <{self.dp_prefix}>
            SELECT DISTINCT ?FMParam
            WHERE {{
                ?potFM a cl:FailureMode .
                <{searchSkillIri}> a cl:Function .
                ?potFM op:preventsFunction <{searchSkillIri}>;
                       dp:hasFailureModeParams ?FMParam .
            }}
        """
        res = self.graph.query(query)

        rows = []
        for (fmparam,) in res:
            # fmparam ist eine IRI; Lese dazu (optional) den aktuellen Wert, wenn im KG als Datenwert gespeichert.
            # Falls die Werte NICHT im KG stehen, kannst du hier nur "id" liefern.
            node_id = str(fmparam)

            # Dummy: t/v als "unbekannt", wenn kein konkreter Wert im KG steht.
            # Wenn du im KG z. B. dp:nodeValue / dp:nodeType ablegst, kannst du die hier mit einem weiteren Graph-Lookup holen.
            t = "string"
            v = ""

            rows.append({"id": node_id, "t": t, "v": v})

        return json.dumps({"rows": rows}, ensure_ascii=False)