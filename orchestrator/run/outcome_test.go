package run

import "testing"

func TestClassifyRunOutcomeScenarioStates(t *testing.T) {
	scenario := authoritativeFixture()
	passing := &Result{
		Verdict:            VerdictValid,
		Config:             RunConfig{Scenario: &scenario},
		ScenarioEvaluation: &ScenarioEvaluation{OK: true, CompletePrimarySLOs: true},
	}
	if outcome, _ := classifyRunOutcome(passing, GateInput{}); outcome != OutcomePass {
		t.Fatalf("passing outcome=%s", outcome)
	}
	passing.ScenarioEvaluation.OK = false
	passing.ScenarioEvaluation.Cause = "delivery below SLO"
	if outcome, _ := classifyRunOutcome(passing, GateInput{}); outcome != OutcomeFail {
		t.Fatalf("SLO failure outcome=%s", outcome)
	}
	passing.ScenarioEvaluation.CompletePrimarySLOs = false
	passing.ScenarioEvaluation.MissingPrimarySLOs = []string{"client_input/loss_tolerant"}
	passing.ScenarioEvaluation.OK = true
	if outcome, _ := classifyRunOutcome(passing, GateInput{}); outcome != OutcomeInconclusive {
		t.Fatalf("diagnostic outcome=%s", outcome)
	}
	if outcome, _ := classifyRunOutcome(passing, GateInput{SUTFailureReasons: []string{"server crashed"}}); outcome != OutcomeFail {
		t.Fatalf("crash outcome=%s", outcome)
	}
	passing.Verdict = VerdictInvalid
	passing.InvalidReasons = []string{"sampler failed: permission denied", "server exit_code=1"}
	if outcome, _ := classifyRunOutcome(passing, GateInput{SUTFailureReasons: []string{"server crashed"}}); outcome != OutcomeInvalid {
		t.Fatalf("independently invalid crash outcome=%s", outcome)
	}
}

func TestClassifyRunOutcomeUnsupportedIsTerminal(t *testing.T) {
	result := &Result{Outcome: OutcomeUnsupported, OutcomeReasons: []string{"scenario not advertised"}}
	if outcome, _ := classifyRunOutcome(result, GateInput{}); outcome != OutcomeUnsupported {
		t.Fatalf("outcome=%s", outcome)
	}
}
