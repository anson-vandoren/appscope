package run

import (
	"bytes"
	"crypto/md5"
	"fmt"
	"io/ioutil"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
	"time"

	"github.com/criblio/scope/internal"
	"github.com/criblio/scope/util"

	"github.com/stretchr/testify/assert"
)

func TestCreateLdscope(t *testing.T) {
	tmpLdscope := filepath.Join(".foo", "ldscope")
	os.Setenv("SCOPE_HOME", ".foo")
	internal.InitConfig()
	err := createLdscope()
	os.Unsetenv("SCOPE_HOME")
	assert.NoError(t, err)

	wb, _ := Asset("build/ldscope")
	hash1 := md5.Sum(wb)
	fb, _ := ioutil.ReadFile(tmpLdscope)
	hash2 := md5.Sum(fb)
	assert.Equal(t, hash1, hash2)

	fb, _ = ioutil.ReadFile("../build/ldscope")
	hash3 := md5.Sum(fb)
	assert.Equal(t, hash2, hash3)

	// Call again, should use cached copy
	err = createLdscope()
	assert.NoError(t, err)

	os.Chtimes(tmpLdscope, time.Now(), time.Now())
	ldscopeInfo, _ := AssetInfo("build/ldscope")
	stat, _ := os.Stat(tmpLdscope)
	assert.NotEqual(t, ldscopeInfo.ModTime(), stat.ModTime())
	err = createLdscope()
	assert.NoError(t, err)
	stat, _ = os.Stat(tmpLdscope)
	assert.Equal(t, ldscopeInfo.ModTime(), stat.ModTime())

	os.RemoveAll(".foo")

}

func TestCreateAll(t *testing.T) {
	os.MkdirAll(".foo", 0755)
	CreateAll(".foo")
	files := []string{"ldscope", "libscope.so", "scope.yml", "scope_protocol.yml"}
	perms := []os.FileMode{0755, 0755, 0644, 0644}
	for i, f := range files {
		path := fmt.Sprintf(".foo/%s", f)
		stat, _ := os.Stat(path)
		assert.Equal(t, stat.Mode(), perms[i])
		wb, _ := Asset(fmt.Sprintf("build/%s", f))
		hash1 := md5.Sum(wb)
		fb, _ := ioutil.ReadFile(path)
		hash2 := md5.Sum(fb)
		assert.Equal(t, hash1, hash2)

		fb, _ = ioutil.ReadFile(fmt.Sprintf("../build/%s", f))
		hash3 := md5.Sum(fb)
		assert.Equal(t, hash2, hash3)
	}
	os.RemoveAll(".foo")
}

func TestEnvironNoScope(t *testing.T) {
	hasScope := func(testArr []string) bool {
		for _, s := range testArr {
			if strings.HasPrefix(s, "SCOPE") {
				return true
			}
		}
		return false
	}
	assert.False(t, hasScope(os.Environ()))
	os.Setenv("SCOPE_FOO", "true")
	assert.True(t, hasScope(os.Environ()))
	assert.False(t, hasScope(environNoScope()))
	os.Unsetenv("SCOPE_FOO")
	assert.False(t, hasScope(os.Environ()))
}

func TestMain(m *testing.M) {
	// Borrowed from http://cs-guy.com/blog/2015/01/test-main/
	switch os.Getenv("TEST_MAIN") {
	case "runpassthrough":
		internal.InitConfig()
		rc := Config{Passthrough: true}
		rc.Run([]string{"/bin/echo", "true"})
	case "createWorkDir":
		internal.InitConfig()
		rc := Config{}
		rc.createWorkDir("foo")
	case "run":
		internal.InitConfig()
		rc := Config{}
		rc.Run([]string{"/bin/echo", "true"})
	default:
		os.Exit(m.Run())
	}
}

// Simplest test, runs passthrough and outputs to stderr
// Passthrough sends environment directly to scope, rather
// than clearing it and creating configuration files
func TestRunPassthrough(t *testing.T) {
	// Test Passthrough, read from Stderr
	cmd := exec.Command(os.Args[0])
	cmd.Env = append(os.Environ(), "TEST_MAIN=runpassthrough", "SCOPE_METRIC_DEST=file://stderr")
	var outb, errb bytes.Buffer
	cmd.Stdout = &outb
	cmd.Stderr = &errb
	err := cmd.Run()
	assert.NoError(t, err)
	assert.Equal(t, "true\n", outb.String())
	assert.Contains(t, errb.String(), "proc.start")
}

func TestCreateWorkDir(t *testing.T) {
	// Test CreateWorkDir, fail first
	f, err := os.OpenFile(".test", os.O_RDONLY|os.O_CREATE, 0400)
	assert.NoError(t, err)
	f.Close()
	cmd := exec.Command(os.Args[0])
	cmd.Env = append(os.Environ(), "TEST_MAIN=createWorkDir", "SCOPE_HOME=.test", "SCOPE_TEST=true")
	err = cmd.Run()
	assert.Error(t, err, "exit status 1")
	os.Remove(".test")

	// Test CreateWorkDir, successfully
	cmd = exec.Command(os.Args[0])
	cmd.Env = append(os.Environ(), "TEST_MAIN=createWorkDir", "SCOPE_HOME=.test", "SCOPE_TEST=true")
	var outb, errb bytes.Buffer
	cmd.Stdout = &outb
	cmd.Stderr = &errb
	err = cmd.Run()
	assert.NoError(t, err, errb.String())
	files, err := ioutil.ReadDir(".test/history")
	matched := false
	wdbase := fmt.Sprintf("%s_%d_%d", "foo", 1, cmd.Process.Pid)
	var wd string
	for _, f := range files {
		if strings.HasPrefix(f.Name(), wdbase) {
			matched = true
			wd = fmt.Sprintf("%s%s", ".test/history/", f.Name())
			break
		}
	}
	assert.True(t, matched)
	exists := util.CheckFileExists(wd)
	assert.True(t, exists)
	os.RemoveAll(".test")
}

func testDefaultScopeConfigYaml(wd string, verbosity int) string {
	wd, _ = filepath.Abs(wd)
	expectedYaml := `metric:
  enable: true
  format:
    type: ndjson
    verbosity: VERBOSITY
  transport:
    type: file
    path: METRICSPATH
    buffering: line
    tls:
      enable: false
      validateserver: false
      cacertpath: ""
event:
  enable: true
  format:
    type: ndjson
  transport:
    type: file
    path: EVENTSPATH
    buffering: line
    tls:
      enable: false
      validateserver: false
      cacertpath: ""
  watch:
  - type: file
    name: '[\s\/\\\.]log[s]?[\/\\\.]?'
    value: .*
  - type: console
    name: (stdout|stderr)
    value: .*
  - type: http
    name: .*
    field: .*
    value: .*
  - type: net
    name: .*
    field: .*
    value: .*
  - type: fs
    name: .*
    field: .*
    value: .*
  - type: dns
    name: .*
    field: .*
    value: .*
libscope:
  configevent: false
  summaryperiod: 10
  commanddir: CMDDIR
  log:
    level: warning
    transport:
      type: file
      path: LDSCOPELOGPATH
      buffering: line
      tls:
        enable: false
        validateserver: false
        cacertpath: ""
`

	expectedYaml = strings.Replace(expectedYaml, "VERBOSITY", strconv.Itoa(verbosity), 1)
	expectedYaml = strings.Replace(expectedYaml, "METRICSPATH", filepath.Join(wd, "metrics.json"), 1)
	expectedYaml = strings.Replace(expectedYaml, "EVENTSPATH", filepath.Join(wd, "events.json"), 1)
	expectedYaml = strings.Replace(expectedYaml, "CMDDIR", filepath.Join(wd, "cmd"), 1)
	expectedYaml = strings.Replace(expectedYaml, "LDSCOPELOGPATH", filepath.Join(wd, "ldscope.log"), 1)
	return expectedYaml
}

func TestSetupWorkDir(t *testing.T) {
	os.Setenv("SCOPE_TEST", "true")
	rc := Config{}
	rc.now = func() time.Time { return time.Unix(0, 0) }
	rc.setupWorkDir([]string{"/bin/foo"})
	wd := fmt.Sprintf("%s_%d_%d_%d", ".foo/history/foo", 1, os.Getpid(), 0)
	exists := util.CheckFileExists(wd)
	assert.True(t, exists)

	argsJSONBytes, err := ioutil.ReadFile(filepath.Join(wd, "args.json"))
	assert.NoError(t, err)
	assert.Equal(t, `["/bin/foo"]`, string(argsJSONBytes))

	expectedYaml := testDefaultScopeConfigYaml(wd, 4)

	scopeYAMLBytes, err := ioutil.ReadFile(filepath.Join(wd, "scope.yml"))
	assert.NoError(t, err)
	assert.Equal(t, expectedYaml, string(scopeYAMLBytes))

	cmdDirExists := util.CheckFileExists(filepath.Join(wd, "cmd"))
	assert.True(t, cmdDirExists)

	payloadsDirExists := util.CheckFileExists(filepath.Join(wd, "payloads"))
	assert.True(t, payloadsDirExists)
	os.RemoveAll(".foo")
	os.Unsetenv("SCOPE_TEST")
}

func TestRun(t *testing.T) {
	// Test SetupWorkDir, successfully
	cmd := exec.Command(os.Args[0])
	cmd.Env = append(os.Environ(), "TEST_MAIN=run", "SCOPE_HOME=.test", "SCOPE_TEST=true")
	err := cmd.Run()
	assert.NoError(t, err)
	files, err := ioutil.ReadDir(".test/history")
	matched := false
	wdbase := fmt.Sprintf("%s_%d_%d", "echo", 1, cmd.Process.Pid)
	var wd string
	for _, f := range files {
		if strings.HasPrefix(f.Name(), wdbase) {
			matched = true
			wd = fmt.Sprintf("%s%s", ".test/history/", f.Name())
			break
		}
	}
	assert.True(t, matched)

	exists := util.CheckFileExists(wd)
	assert.True(t, exists)

	argsJSONBytes, err := ioutil.ReadFile(filepath.Join(wd, "args.json"))
	assert.NoError(t, err)
	assert.Equal(t, `["/bin/echo","true"]`, string(argsJSONBytes))

	expectedYaml := testDefaultScopeConfigYaml(wd, 4)

	scopeYAMLBytes, err := ioutil.ReadFile(filepath.Join(wd, "scope.yml"))
	assert.NoError(t, err)
	assert.Equal(t, expectedYaml, string(scopeYAMLBytes))

	cmdDirExists := util.CheckFileExists(filepath.Join(wd, "cmd"))
	assert.True(t, cmdDirExists)
	os.RemoveAll(".test")
}
