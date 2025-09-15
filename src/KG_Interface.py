# kg_interface.py
from rdflib import Graph, URIRef
from rdflib.namespace import RDF
from typing import Sequence

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
    
    def ingestOccuredFailure(
        self,
        id: str,
        failureModeIRI: str,
        monActIRI: Sequence[str],
        srIRI: str,   # akzeptiert tuple oder list
        lastSkillName: str,
        lastProcessName: str,
        summary: str,
        plcSnapshot: str,
    ) -> bool:
        def _to_list(x):
            if x is None:
                return []
            if isinstance(x, str):
                return [x]
            try:
                return list(x)
            except TypeError:
                return [x]

        def _print_param(name, value):
            if isinstance(value, str) or value is None:
                print(f"{name}: {value}")
            else:
                items = _to_list(value)
                print(f"{name} (len={len(items)}):")
                for i, v in enumerate(items, 1):
                    print(f"  {i}: {v}")

        def _print_list(name, items):
            print(f"{name} (len={len(items)}):")
            for i, v in enumerate(items, 1):
                print(f"  {i}: {v}")

        # Debug: Eingaben ausgeben
        print("---PYTHON----")
        _print_param("id", id)
        _print_param("failureModeIRI", failureModeIRI)
        _print_param("monActIRI", monActIRI)
        _print_param("srIRI", srIRI)
        _print_param("lastSkillName", lastSkillName)
        _print_param("lastProcessName", lastProcessName)
        _print_param("summary", summary)
        _print_param("plcSnapshot", plcSnapshot)

        # Separator abhängig von self.ont_iri
        base_sep = '' if self.ont_iri.endswith(('#','/')) else '#'

        # Hilfsfunktion zum Erzeugen der neuen IDs
        def _make_ids(marker: str, n: int) -> list[str]:
            # i startet bei 1
            return [f"{self.ont_iri}{base_sep}{marker}{id}_{i}" for i in range(1, n + 1)]

        # Längen bestimmen (robust gegen str vs. list/tuple)
        n_m  = len(_to_list(monActIRI))

        # Neue IRI-Listen erzeugen
        if (failureModeIRI != ''):
            fm_ids = _make_ids("FM_", 1)   # für failureModeIRI
        m_ids  = _make_ids("M_",  n_m)    # für monActIRI
        if (srIRI != ''):
            sr_ids = _make_ids("SR_", 1)   # für srIRIs
        

        # Debug: neue Listen ausgeben
        _print_list("fm_id", fm_ids)
        _print_list("m_ids",  m_ids)
        _print_list("sr_id", sr_ids)
        print("---PYTHON----")

        # TODO: falls benötigt, hier mit fm_ids/m_ids/sr_ids weiterarbeiten
        return True