# SysMonitor

Dashboard de monitoramento de sistema para Windows, desenvolvido em C, HTML, CSS e JavaScript.

O projeto coleta metricas reais do computador por meio de APIs nativas do Windows e exibe os dados em um painel web atualizado em tempo real.

## Funcionalidades

- Uso geral de CPU em tempo real
- Uso de memoria RAM
- Uso de disco
- Tráfego de rede enviado e recebido
- Lista de processos ativos
- Uso de CPU por processo
- Uso de memoria por processo
- API HTTP local em `http://localhost:8888/metrics`
- Dashboard web com atualização automática a cada 2 segundos

## Tecnologias Utilizadas

- C
- Windows API
- Winsock
- PSAPI
- IP Helper API
- HTML5
- CSS3
- JavaScript
- Canvas API para gráficos

## Estrutura do Projeto

```txt
.
├── backend/
│   └── sysmonitor_api.c
├── CSS/
│   └── style.css
├── html/
│   ├── index.html
│   ├── sobre.html
│   ├── download.html
│   └── sysdata.json
├── js/
│   ├── dashboard.js
│   └── charts.js
├── .gitignore
└── README.md
```

## Requisitos

- Windows 10 ou Windows 11
- GCC via MinGW-w64/MSYS2
- Navegador moderno, como Chrome, Edge ou Firefox

## Instalacao do GCC

Uma forma recomendada e instalar o MSYS2:

```txt
https://www.msys2.org/
```

Depois, no terminal MSYS2 MINgw64, instale o GCC:

```bash
pacman -S mingw-w64-ucrt-x86_64-gcc
```

Verifique se o GCC esta disponivel:

```powershell
gcc --version
```

## Como Compilar

Na raiz do projeto, execute:

```powershell
gcc backend\sysmonitor_api.c -o sysmonitor.exe -lws2_32 -lpdh -liphlpapi -lpsapi -lntdll -O2
```

## Como Executar

Depois de compilar, rode:

```powershell
.\sysmonitor.exe
```

Se tudo estiver certo, o terminal mostrará a API local:

```txt
http://localhost:8888/metrics
```

Mantenha esse terminal aberto enquanto usa o dashboard.

## Como Abrir o Dashboard

Com o `sysmonitor.exe` rodando, abra no navegador:

```txt
html/index.html
```

O arquivo `js/dashboard.js` consulta a API local a cada 2 segundos:

```txt
http://localhost:8888/metrics
```

Caso a API não esteja rodando, o dashboard tenta usar o arquivo de exemplo:

```txt
html/sysdata.json
```

## Endpoint da API

### `GET /metrics`

Retorna um JSON com os dados atuais do sistema:

```json
{
  "cpu": {
    "usage_pct": 12.5,
    "cores": 8
  },
  "ram": {
    "usage_pct": 64.2,
    "used_bytes": 6800000000,
    "total_bytes": 16000000000
  },
  "disk": {
    "usage_pct": 47.0,
    "free_bytes": 120000000000,
    "total_bytes": 256000000000
  },
  "network": {
    "send_bytes_sec": 1200,
    "recv_bytes_sec": 5400,
    "sent_bytes_total": 30000000,
    "recv_bytes_total": 290000000,
    "adapter_count": 1
  },
  "processes": [
    {
      "pid": 1234,
      "name": "chrome.exe",
      "cpu_pct": 3.4,
      "ram_bytes": 250000000
    }
  ]
}
```

## APIs do Windows Utilizadas

- `GetSystemTimes`: coleta uso geral de CPU
- `GlobalMemoryStatusEx`: coleta uso de memoria RAM
- `GetDiskFreeSpaceEx`: coleta uso de disco
- `GetIfTable`: coleta dados de rede
- `CreateToolhelp32Snapshot`: lista processos ativos
- `GetProcessMemoryInfo`: coleta memoria por processo
- `GetProcessTimes`: calcula CPU por processo
- `Winsock`: cria o servidor HTTP local

## Observacoes

- O uso de CPU geral e por processo precisa de duas leituras para calcular diferenca. Por isso, na primeira chamada pode aparecer `0.0%`.
- O executavel `sysmonitor.exe` nao deve ser versionado no Git, pois e gerado pela compilacao.
- A pasta `.vscode/` tambem fica fora do Git porque contem configuracoes locais do editor.
- O projeto foi feito para Windows. Em Linux ou macOS, as APIs usadas no backend nao estao disponiveis.

## Autores

- Clovis da Silva
- Jeferson Otondo
- Laura Layslla
- Vitor Almeida
