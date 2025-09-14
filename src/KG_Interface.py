# kg_interface.py
from rdflib import Graph, URIRef
from rdflib.namespace import RDF
import json

class KGInterface:
    def __init__(self):
        self.ontology_path = r"C:\Users\Alexander Verkhov\OneDrive\Dokumente\MPA\Implementierung_MPA\Test\src\FMEA_KG.ttl"
        self.ont_iri = "http://www.semanticweb.org/FMEA_VDA_AIAG_2021/"
        self.class_prefix = self.ont_iri + "class_"
        self.op_prefix = self.ont_iri + "op_"
        self.dp_prefix = self.ont_iri + "dp_"
        self.graph = Graph()
        # TTL laden
        self.graph.parse(self.ontology_path, format="turtle")

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
        output_lines = []
        for row in res:  # row ist rdflib.query.ResultRow
            potFM = str(row["potFM"])    # URIRef → str
            fmparam = str(row["FMParam"])
            # Platzhalter für Typ/Wert (falls später im KG abgelegt):
            #print(f"potFM: {potFM}    FMParam: {fmparam}")
            output_lines.append(potFM)
            output_lines.append(fmparam)

        return "\n".join(output_lines)
    
    def getMonitoringActionForFailureMode(self, FMIri: str) -> str:
        base_sep = '' if self.ont_iri.endswith(('#','/')) else '#'
        defaultIri = self.ont_iri + base_sep + "checkParameters"
        query = f"""
            PREFIX cl: <{self.class_prefix}>
            PREFIX op: <{self.op_prefix}>
            PREFIX dp: <{self.dp_prefix}>
            SELECT DISTINCT ?monAct ?monActParams
            WHERE {{
                <{FMIri}> a cl:FailureMode .
                ?monAct a cl:MonitoringAction ;
                        op:monitorsFailureMode <{FMIri}> ;
                        dp:hasMonActParams ?monActParams .
                FILTER( ?monAct != IRI("{defaultIri}") )
            }}
        """
        #print(query)
        res = self.graph.query(query)

        output_lines = []
        for row in res:
            sysR   = str(row["monAct"])
            params = str(row["monActParams"])
            output_lines.append(sysR)
            output_lines.append(params)

        return "\n".join(output_lines)
    
    def getSystemreactionForFailureMode(self, FMIri: str) -> str:
        query = f"""
            PREFIX cl: <{self.class_prefix}>
            PREFIX op: <{self.op_prefix}>
            PREFIX dp: <{self.dp_prefix}>
            SELECT DISTINCT ?sysReact ?SysReactParams
            WHERE {{
                <{FMIri}> a cl:FailureMode .
                ?sysReact a cl:SystemReaction ;
                        op:reactsOnFailureMode <{FMIri}> ;
                        dp:hasSysReactParams ?SysReactParams .
            }}
        """
        res = self.graph.query(query)

        output_lines = []
        for row in res:
            sysR   = str(row["sysReact"])
            params = str(row["SysReactParams"])
            output_lines.append(sysR)
            output_lines.append(params)

        return "\n".join(output_lines)