# CLAUDE.md

## Visão Geral do Projeto

Este projeto implementa uma nova action para o `ipfw` no FreeBSD, com o objetivo de adicionar o endereço de origem ou destino de um pacote em uma **table** do `ipfw`, caso o pacote dê match em uma regra.

**Versão alvo:** FreeBSD 15/stable (primeira versão)

---

## O que é e como funciona

O `ipfw` é o firewall nativo do FreeBSD. Ele suporta **tables** — estruturas de dados que armazenam conjuntos de endereços IP, redes, portas ou outros valores, usadas para correspondência dinâmica em regras.

A nova action `addtable` (nome sujeito a definição final) permite que, ao invés de apenas aceitar ou bloquear um pacote, uma regra possa **inserir dinamicamente** o endereço IP de origem (`src`) ou destino (`dst`) do pacote em uma table especificada.

Exemplo de uso pretendido:

```
ipfw add 100 addtable 10 src from any to any
ipfw add 200 addtable 10 dst from any to 192.168.1.1
```

---

## Estrutura do Projeto

O código será desenvolvido como uma modificação/extensão do kernel do FreeBSD e das ferramentas de userspace do `ipfw`.

### Componentes principais esperados

| Componente | Localização (árvore FreeBSD) | Descrição |
|---|---|---|
| Kernel - lógica de regras | `sys/netpfil/ipfw/ip_fw_rules.c` | Processamento de regras e actions |
| Kernel - tables | `sys/netpfil/ipfw/ip_fw_table.c` | Manipulação de tables no kernel |
| Userspace - ipfw CLI | `sbin/ipfw/ipfw2.c` | Parser de comandos e montagem de regras |
| Userspace - tables CLI | `sbin/ipfw/tables.c` | Comandos relacionados a tables |

---

## Ambiente de Desenvolvimento

- **Sistema operacional:** FreeBSD 15/stable
- **Compilador:** clang (padrão do FreeBSD base)
- **Build system:** `make` com o sistema de build do FreeBSD (`src.conf`, `make.conf`)

### Configuração do ambiente

1. Obter o código-fonte do FreeBSD 15/stable:
   ```sh
   git clone --branch stable/15 https://git.freebsd.org/src.git /usr/src
   ```

2. Para compilar apenas o kernel:
   ```sh
   cd /usr/src
   make -j$(sysctl -n hw.ncpu) buildkernel KERNCONF=GENERIC
   ```

3. Para compilar apenas o userspace do `ipfw`:
   ```sh
   cd /usr/src/sbin/ipfw
   make
   ```

---

## Convenções de Código

- Seguir o estilo de código do FreeBSD: [FreeBSD style(9)](https://man.freebsd.org/cgi/man.cgi?style)
- Indentação com **tabs** de 8 espaços
- Nomes de funções e variáveis em `snake_case`
- Comentários em inglês (padrão da base do FreeBSD)
- Novos opcodes de action devem ser adicionados em `ip_fw.h` com prefixo `O_`

---

## Referências

- [ipfw(8) man page](https://man.freebsd.org/cgi/man.cgi?ipfw)
- [FreeBSD Handbook - Firewalls](https://docs.freebsd.org/en/books/handbook/firewalls/)
- Código-fonte do ipfw: `sys/netpfil/ipfw/` e `sbin/ipfw/` na árvore do FreeBSD
