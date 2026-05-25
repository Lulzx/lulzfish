const content = document.getElementById("content");

const resultClass = (game) => {
  if (game.lulzfishScore === 1) return "win";
  if (game.lulzfishScore === 0.5) return "draw";
  return "loss";
};

const fmt = (value) => new Intl.NumberFormat("en-US").format(value);

function pct(part, total) {
  return `${(100 * part) / Math.max(1, total)}%`;
}

function renderSummary(data) {
  const winCount = data.games.filter((game) => game.lulzfishScore === 1).length;
  const drawCount = data.games.filter((game) => game.lulzfishScore === 0.5).length;
  const lossCount = data.games.filter((game) => game.lulzfishScore === 0).length;
  const date = new Date(data.generatedAt);

  return `
    <div class="meta">
      <span class="pill">Generated ${date.toLocaleString()}</span>
      <span class="pill">Depth ${data.engineDepth}</span>
      <span class="pill">${data.gamesPerOpponent} games per opponent</span>
      <span class="pill">Max ${data.maxPlies} plies</span>
      <span class="pill">Adjudication ${data.materialAdjudicationCp} cp</span>
    </div>
    <section class="summary">
      <div class="metric">
        <span>Total score</span>
        <strong>${data.totalScoreText}</strong>
      </div>
      <div class="metric">
        <span>Record</span>
        <strong>${winCount}-${drawCount}-${lossCount}</strong>
      </div>
      <div class="metric">
        <span>Elapsed</span>
        <strong>${data.elapsedSeconds}s</strong>
      </div>
    </section>
  `;
}

function renderOpponent(opponent) {
  const elo = opponent.estimatedEloDiff === null ? "n/a" : `${opponent.estimatedEloDiff > 0 ? "+" : ""}${opponent.estimatedEloDiff}`;

  return `
    <article class="card">
      <div class="card-head">
        <h2>${opponent.name}</h2>
        <span class="style">${opponent.style}</span>
      </div>
      <p class="desc">${opponent.description}</p>
      <div class="score-row">
        <div class="score">${opponent.scoreText}</div>
        <div class="bar" aria-label="Result split">
          <div class="win" style="width: ${pct(opponent.wins, opponent.games)}"></div>
          <div class="draw" style="width: ${pct(opponent.draws, opponent.games)}"></div>
          <div class="loss" style="width: ${pct(opponent.losses, opponent.games)}"></div>
        </div>
      </div>
      <div class="stats">
        <div class="stat">W-D-L<strong>${opponent.wins}-${opponent.draws}-${opponent.losses}</strong></div>
        <div class="stat">Expected<strong>${Math.round(opponent.expectedScore * 100)}%</strong></div>
        <div class="stat">Elo diff<strong>${elo}</strong></div>
        <div class="stat">Avg material<strong>${opponent.avgMaterialCp > 0 ? "+" : ""}${fmt(opponent.avgMaterialCp)}</strong></div>
      </div>
    </article>
  `;
}

function renderGames(games) {
  return `
    <section class="games">
      <h2>Game Log</h2>
      <table>
        <thead>
          <tr>
            <th>Opponent</th>
            <th>Game</th>
            <th>Color</th>
            <th>Opening</th>
            <th>Result</th>
            <th>Plies</th>
            <th>Material</th>
            <th>Moves</th>
          </tr>
        </thead>
        <tbody>
          ${games.map((game) => `
            <tr>
              <td>${game.opponent}</td>
              <td>${game.game}</td>
              <td>${game.lulzfishColor}</td>
              <td>${game.opening}</td>
              <td><span class="${resultClass(game)}">${game.result}</span></td>
              <td>${game.plies}</td>
              <td>${game.materialCpForLulzfish > 0 ? "+" : ""}${fmt(game.materialCpForLulzfish)}</td>
              <td class="moves">${game.moves.slice(0, 22).join(" ")}${game.moves.length > 22 ? " ..." : ""}</td>
            </tr>
          `).join("")}
        </tbody>
      </table>
    </section>
  `;
}

async function load() {
  try {
    const response = await fetch("./data.json", { cache: "no-store" });
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    const data = await response.json();
    content.innerHTML = `
      ${renderSummary(data)}
      <section class="grid">
        ${data.opponents.map(renderOpponent).join("")}
      </section>
      ${renderGames(data.games)}
    `;
  } catch (error) {
    content.innerHTML = `<div class="error">Could not load benchmark data: ${error.message}</div>`;
  }
}

load();
