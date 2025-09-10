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
        """Gibt rows-JSON zurück: [{"potFM","FMParam","t","v"} ...]"""
        base_sep = '' if self.ont_iri.endswith(('#','/')) else '#'
        searchSkillIri = self.ont_iri + base_sep + interruptedSkill

        query = f"""
            PREFIX cl: <{self.class_prefix}>
            PREFIX op: <{self.op_prefix}>
            PREFIX dp: <{self.dp_prefix}>
            SELECT DISTINCT ?potFM ?FMParam
            WHERE {{
                ?potFM a cl:FailureMode ;
                       op:preventsFunction <{searchSkillIri}> ;
                       dp:hasFailureModeParams ?FMParam .
                <{searchSkillIri}> a cl:Function .
            }}
        """
        res = self.graph.query(query)

        rows = []
        for row in res:  # row ist rdflib.query.ResultRow
            potFM = str(row["potFM"])    # URIRef → str
            fmparam = str(row["FMParam"])
            # Platzhalter für Typ/Wert (falls später im KG abgelegt):
            #print(f"potFM: {potFM}    FMParam: {fmparam}")
            rows.append({"potFM": potFM, "FMParam": fmparam, "t": "string", "v": ""})

        return json.dumps({"rows": rows}, ensure_ascii=False)